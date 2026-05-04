#include "dog_peripherals.h"
#include "config.h"
#include "audio_clips.h"
#include "paulbot_audio.h"
#include "font5x7.h"

#ifdef DISP_MOSI_GPIO

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include <string.h>
#include <math.h>

static const char *TAG = "DOG_PERIPH";

static volatile bool force_blink = false;
static volatile int eye_mood = 0; // 0=happy (full), 1=angry, 2=neutral, 3=sad
static QueueHandle_t button_evt_queue = NULL;

static char oled_text_msg[32] = {0};
static volatile int oled_text_timer = 0;

void dog_set_eye_mood(int mood) {
    if (mood >= 0 && mood <= 3) eye_mood = mood;
}

void dog_set_oled_text(const char* msg, int duration_ms) {
    strncpy(oled_text_msg, msg, sizeof(oled_text_msg) - 1);
    oled_text_timer = duration_ms / 60; // ~60ms per frame
}

// --- OLED SPI Display ---
static esp_lcd_panel_handle_t panel_handle = NULL;

// Helper: pack RGB565 (5-6-5 big-endian for ST7789)
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Simple fast pseudo-random (xorshift) – cheaper than rand() per-pixel
static uint32_t xor_state = 123456789;
static inline uint32_t fast_rand(void) {
    xor_state ^= xor_state << 13;
    xor_state ^= xor_state >> 17;
    xor_state ^= xor_state << 5;
    return xor_state;
}

static void dog_eyes_task(void *arg) {
    uint16_t *buffer = malloc(160 * 80 * sizeof(uint16_t));
    if (!buffer) {
        ESP_LOGE(TAG, "No mem for eyes");
        vTaskDelete(NULL);
    }

    int blink_timer = 0;
    int next_blink = 50 + (rand() % 100);
    bool blinking = false;
    int frame_count = 0;

    // Shared gaze for both eyes
    static int pupil_dx = 0;
    static int pupil_dy = 0;
    static int pupil_target_dx = 0;
    static int pupil_target_dy = 0;

    // Shared color wander for both eyes (hue phase)
    static int color_phase = 0;
    static int target_phase = 0;

    /* ---- Eye geometry (two eyes side-by-side on 160x80) ---- */
    // Left eye center, Right eye center
    const int eye_cx[2] = { 40, 120 };
    const int eye_cy = 40;
    const int eye_rx = 34;  // horizontal radius of each eye ellipse
    const int base_ry = 32; // vertical radius (before blink)
    const int iris_r  = 18; // iris radius
    const int base_pupil_r = 7; // base pupil radius

    while (1) {
        frame_count++;

        /* ---- Blink logic ---- */
        blink_timer++;
        if (!blinking && blink_timer > next_blink) {
            blinking = true;
            blink_timer = 0;
        }
        if (force_blink) {
            blinking = true;
            blink_timer = 0;
            force_blink = false;
        }

        int max_ry = base_ry;
        if (blinking) {
            max_ry = 3;
            if (blink_timer > 1) {
                blinking = false;
                blink_timer = 0;
                next_blink = 40 + (rand() % 100);
            }
        }

        /* ---- Gaze / pupil wander ---- */
        if (rand() % 20 == 0) {
            pupil_target_dx = (rand() % 14) - 7;   // ±7 horizontal
            pupil_target_dy = (rand() % 8) - 4;    // ±4 vertical
        }
        if (pupil_dx < pupil_target_dx) pupil_dx++;
        if (pupil_dx > pupil_target_dx) pupil_dx--;
        if (pupil_dy < pupil_target_dy) pupil_dy++;
        if (pupil_dy > pupil_target_dy) pupil_dy--;

        /* ---- Color wander (hue shift multiplier) ---- */
        if (rand() % 150 == 0) {
            target_phase = (rand() % 101) - 50; // -50 to +50
        }
        // Change by 1 unit only every 4th frame for ultra-subtle speed
        if (frame_count % 4 == 0) {
            if (color_phase < target_phase) color_phase++;
            if (color_phase > target_phase) color_phase--;
        }
        int g_mult = 256 - color_phase; // shift green down/up
        int b_mult = 256 + color_phase; // shift blue up/down

        /* ---- Pupil breathing / dilation (subtle ±1 px oscillation) ---- */
        int pupil_r = base_pupil_r + ((frame_count / 12) % 3) - 1; // 6,7,8 cycle
        int pupil_r_sq = pupil_r * pupil_r;

        /* ---- Pre-compute frame-invariant constants (avoid re-calc per pixel) ---- */
        int rx_sq = eye_rx * eye_rx;           // 34*34 = 1156
        int ry_sq = max_ry * max_ry;           // max 32*32 = 1024
        int ellipse_limit = rx_sq * ry_sq;     // max ~1,183,744 (fits int32 easily)
        int iris_r_sq = iris_r * iris_r;       // 18*18 = 324
        uint16_t warm_edge = rgb565(240, 230, 220);
        uint16_t faint_catchlight = rgb565(200, 220, 255);

        /* ---- Render both eyes ---- */
        if (oled_text_timer > 0) {
            oled_text_timer--;
            memset(buffer, 0, 160 * 80 * sizeof(uint16_t));
            int msg_len = strlen(oled_text_msg);
            int scale = 3;
            int fw = 5 * scale;
            int fh = 7 * scale;
            int char_space = 1 * scale;
            int total_w = msg_len * (fw + char_space) - char_space;
            int start_x = (160 - total_w) / 2;
            int start_y = (80 - fh) / 2;
            for (int i = 0; i < msg_len; i++) {
                char c = oled_text_msg[i];
                if (c < 32 || c > 126) c = 32;
                const uint8_t *glyph = &font5x7[(c - 32) * 8];
                int cx = start_x + i * (fw + char_space);
                for (int gx = 0; gx < 5; gx++) {
                    uint8_t col = glyph[gx];
                    for (int gy = 0; gy < 7; gy++) {
                        if (col & (1 << gy)) {
                            for (int dx = 0; dx < scale; dx++) {
                                for (int dy = 0; dy < scale; dy++) {
                                    int px = cx + gx * scale + dx;
                                    int py = start_y + gy * scale + dy;
                                    if (px >= 0 && px < 160 && py >= 0 && py < 80) {
                                        buffer[py * 160 + px] = 0xFFFF; // White text
                                    }
                                }
                            }
                        }
                    }
                }
            }
            vTaskDelay(1);
        } else {
            for (int y = 0; y < 80; y++) {
                int dy = y - eye_cy;
                int dy_sq_rx = dy * dy * rx_sq;  // pre-compute for this row
    
                for (int x = 0; x < 160; x++) {
                    uint16_t color = 0x0000; // background black

                // Spatial early-reject: skip the gap between eyes (x 75-85)
                // and only check the relevant eye based on which half of the screen
                int ei_start, ei_end;
                if (x < 6) { ei_start = 2; ei_end = 2; }       // too far left for either
                else if (x < 75) { ei_start = 0; ei_end = 1; } // only left eye possible
                else if (x < 86) { ei_start = 2; ei_end = 2; } // gap between eyes
                else if (x < 155) { ei_start = 1; ei_end = 2; }// only right eye possible
                else { ei_start = 2; ei_end = 2; }             // too far right

                for (int ei = ei_start; ei < ei_end; ei++) {
                    int dx = x - eye_cx[ei];

                    // Ellipse test (all int32 – max value ~2.4M, well within 2^31)
                    int ex_val = dx * dx * ry_sq + dy_sq_rx;
                    if (ex_val > ellipse_limit)
                        continue; // outside this eye

                    /* ---- Eyelid (mood) ---- */
                    int inner_dx = (ei == 0) ? dx : -dx;

                    int eye_lid_y = -1000;
                    if (eye_mood == 0) {
                        eye_lid_y = -1000; // full happy
                    } else if (eye_mood == 1) {
                        eye_lid_y = eye_cy - 8 - (inner_dx * 3 / 7); // angry
                    } else if (eye_mood == 2) {
                        eye_lid_y = eye_cy - 14; // neutral
                    } else if (eye_mood == 3) {
                        eye_lid_y = eye_cy - 22 + (inner_dx * 3 / 7); // sad
                    }

                    if (y <= eye_lid_y && !blinking)
                        continue;

                    /* ---- Sclera ---- */
                    int dist_sq = dx * dx + dy * dy;
                    color = (dist_sq > rx_sq * 3 / 4) ? warm_edge : 0xFFFF;

                    /* ---- Iris ---- */
                    int idx = dx - pupil_dx;
                    int idy = dy - pupil_dy;
                    int id_sq = idx * idx + idy * idy;

                    if (id_sq <= iris_r_sq) {
                        int frac = id_sq * 256 / iris_r_sq;

                        uint8_t r, g, b;
                        if (frac > 200) {
                            r = 10; g = 50; b = 60;
                        } else if (frac > 120) {
                            r = 20; g = 120; b = 110;
                        } else if (frac > 50) {
                            r = 40; g = 180; b = 160;
                        } else {
                            r = 80; g = 220; b = 200;
                        }

                        uint32_t rr = fast_rand();
                        if ((rr & 0xF) == 0) {
                            r = 200; g = 160; b = 40;
                        }
                        if ((rr & 0x1F) == 1) {
                            r = 5; g = 40; b = 35;
                        }

                        // Apply subtle color wander (hue scaling avoids gradient pixelation)
                        int ig = (g * g_mult) >> 8;
                        int ib = (b * b_mult) >> 8;
                        g = (ig > 255) ? 255 : ig;
                        b = (ib > 255) ? 255 : ib;

                        color = rgb565(r, g, b);

                        /* ---- Pupil ---- */
                        if (id_sq <= pupil_r_sq) {
                            color = 0x0000;
                        } else {
                            /* ---- Catchlight ---- */
                            int cl_ox = (ei == 0) ? 6 : -6;
                            int hx = idx - cl_ox;
                            int hy = idy + 6;
                            if (hx * hx + hy * hy <= 9) {
                                color = 0xFFFF;
                            }
                            int hx2 = idx + ((ei == 0) ? -3 : 3);
                            int hy2 = idy + 4;
                            if (hx2 * hx2 + hy2 * hy2 <= 4) {
                                color = faint_catchlight;
                            }
                        }
                    }

                    break; // pixel claimed by this eye
                }
                buffer[y * 160 + x] = color;
            }

            // Yield every 16 rows so httpd/audio tasks can run
            if ((y & 0xF) == 0xF) {
                vTaskDelay(1);
            }
        }
        }

        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 160, 80, buffer);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static void init_display(void) {
    ESP_LOGI(TAG, "Initializing SPI for OLED");
    spi_bus_config_t buscfg = {
        .sclk_io_num = DISP_CLK_GPIO,
        .mosi_io_num = DISP_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 160 * 80 * 2 + 8
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISP_DC_GPIO,
        .cs_gpio_num = -1,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // Gap for 160x80 ST7789 screens to fix the bottom 15% garbage
    esp_lcd_panel_set_gap(panel_handle, 0, 24);

    // Swap x and y
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, false, true);

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    xTaskCreate(dog_eyes_task, "dog_eyes", 5120, NULL, 1, NULL); // prio 1: yields to httpd & audio
}

// --- Audio PDM & Amp ---
static i2s_chan_handle_t tx_chan = NULL;
static RingbufHandle_t audio_rb = NULL;

static void dog_audio_task(void *arg) {
    size_t w_bytes = 0;
    int16_t silence[512] = {0}; // 1024 bytes of silence
    
    // We need to handle potential byte-alignment issues from the ring buffer.
    uint8_t leftover_byte = 0;
    bool has_leftover = false;
    bool stream_active = true;

    while(1) {
        size_t item_size = 0;
        // Wait 150ms for next chunk. If we don't get one, stream is over.
        uint8_t *item = (uint8_t *)xRingbufferReceive(audio_rb, &item_size, stream_active ? pdMS_TO_TICKS(150) : portMAX_DELAY);
        
        if (item) {
            stream_active = true;
            size_t offset = 0;
            while (offset < item_size) {
                size_t chunk_bytes = item_size - offset;
                
                if (has_leftover) {
                    uint8_t sample_bytes[2] = { leftover_byte, item[offset] };
                    i2s_channel_write(tx_chan, sample_bytes, 2, &w_bytes, portMAX_DELAY);
                    has_leftover = false;
                    offset++;
                    continue;
                }
                
                size_t even_chunk = chunk_bytes & ~1;
                if (even_chunk > 0) {
                    i2s_channel_write(tx_chan, item + offset, even_chunk, &w_bytes, portMAX_DELAY);
                    offset += even_chunk;
                } else {
                    leftover_byte = item[offset];
                    has_leftover = true;
                    offset++;
                }
            }
            vRingbufferReturnItem(audio_rb, (void *)item);
        } else {
            // Stream timed out. Flush the DMA buffers with silence to stop any repeating/looping audio.
            if (stream_active) {
                for (int i = 0; i < 4; i++) {
                    i2s_channel_write(tx_chan, silence, sizeof(silence), &w_bytes, portMAX_DELAY);
                }
                stream_active = false;
                has_leftover = false;
                ESP_LOGI(TAG, "Audio stream finished, DMA flushed.");
            }
        }
    }
}

void dog_audio_play_chunk(const uint8_t *data, size_t size) {
    if (!audio_rb) return;
    size_t sent = 0;
    while (sent < size) {
        size_t to_send = size - sent;
        if (to_send > 1024) to_send = 1024;
        if (xRingbufferSend(audio_rb, data + sent, to_send, pdMS_TO_TICKS(1000)) == pdTRUE) {
            sent += to_send;
        } else {
            ESP_LOGW(TAG, "Audio RB full, retrying");
        }
    }
}

// --- Async audio feeder (frees httpd thread immediately) ---
typedef struct {
    uint8_t *data;
    size_t size;
} audio_payload_t;

static QueueHandle_t audio_payload_queue = NULL;

static void dog_audio_feeder_task(void *arg) {
    audio_payload_t payload;
    while (1) {
        if (xQueueReceive(audio_payload_queue, &payload, portMAX_DELAY) == pdTRUE) {
            dog_audio_play_chunk(payload.data, payload.size);
            free(payload.data);
        }
    }
}

void dog_audio_play_async(uint8_t *data, size_t size) {
    if (!audio_payload_queue || !data || size == 0) {
        free(data);
        return;
    }
    audio_payload_t payload = { .data = data, .size = size };
    if (xQueueSend(audio_payload_queue, &payload, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Audio queue full, dropping payload");
        free(data);
    }
}

void dog_audio_play_tone(void) {
    if (!audio_rb) return;
    ESP_LOGI(TAG, "Playing bark clip...");
    size_t samples = sizeof(clip_bark) / sizeof(clip_bark[0]);
    uint8_t *tone_buf = malloc(2048);
    if (!tone_buf) return;
    
    for (size_t i = 0; i < samples; i++) {
        int16_t val = clip_bark[i];
        int buf_idx = (i % 1024) * 2;
        tone_buf[buf_idx] = val & 0xFF;           // Low byte
        tone_buf[buf_idx + 1] = (val >> 8) & 0xFF; // High byte
        
        if (buf_idx == 2046 || i == samples - 1) {
            dog_audio_play_chunk(tone_buf, buf_idx + 2);
        }
    }
    free(tone_buf);
    ESP_LOGI(TAG, "Bark clip finished");
}

void dog_audio_play_paulbot(void) {
    if (!audio_rb) return;
    ESP_LOGI(TAG, "Playing paulbot boot sound...");
    dog_audio_play_chunk(paulbot_audio, paulbot_audio_len);
    ESP_LOGI(TAG, "Paulbot boot sound finished");
}


static void init_audio(void) {
    ESP_LOGI(TAG, "Initializing PDM Audio");
    gpio_config_t amp_conf = {
        .pin_bit_mask = (1ULL << AUDIO_AMP_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp_conf);
    gpio_set_level(AUDIO_AMP_GPIO, 1);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = AUDIO_CLK_GPIO,
            .dout = AUDIO_DATA_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(tx_chan, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    audio_rb = xRingbufferCreate(16384, RINGBUF_TYPE_BYTEBUF);
    audio_payload_queue = xQueueCreate(16, sizeof(audio_payload_t));

    xTaskCreate(dog_audio_task, "dog_audio", 4096, NULL, 5, NULL);
    xTaskCreate(dog_audio_feeder_task, "audio_feed", 3072, NULL, 4, NULL);
}

// --- Microphone ADC ---
#define MIC_ADC_CHAN ADC_CHANNEL_2 // GPIO 2

static void dog_mic_task(void *arg) {
    adc_oneshot_unit_handle_t adc1_handle = (adc_oneshot_unit_handle_t)arg;
    int adc_raw;
    while(1) {
        adc_oneshot_read(adc1_handle, MIC_ADC_CHAN, &adc_raw);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void init_mic(void) {
    ESP_LOGI(TAG, "Initializing Mic on ADC1 CH2 (GPIO 2)");
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_11,
    };
    esp_err_t err = adc_oneshot_config_channel(adc1_handle, MIC_ADC_CHAN, &config);
    if(err != ESP_OK) { // fallback to 12 if 11 is undefined/fails
        config.atten = ADC_ATTEN_DB_12;
        adc_oneshot_config_channel(adc1_handle, MIC_ADC_CHAN, &config);
    }

    xTaskCreate(dog_mic_task, "dog_mic", 2048, adc1_handle, 4, NULL);
}

// --- Buttons ---
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    if (button_evt_queue) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(button_evt_queue, &gpio_num, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void button_task(void* arg) {
    uint32_t io_num;
    while(1) {
        if(xQueueReceive(button_evt_queue, &io_num, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if(gpio_get_level(io_num) == 0) {
                ESP_LOGI(TAG, "Button %lu pressed", io_num);
                if (io_num == BTN_BOOT_GPIO) {
                    eye_mood = (eye_mood + 1) % 4;
                    dog_audio_play_tone(); // audio feedback
                } else {
                    force_blink = true;
                }
            }
        }
    }
}

static void init_buttons(void) {
    ESP_LOGI(TAG, "Initializing Buttons");
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_MOVE_WAKE_GPIO) | (1ULL << BTN_AUDIO_WAKE_GPIO) | (1ULL << BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_conf);
    
    button_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    esp_err_t err = gpio_install_isr_service(0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR install err: %d", err);
    }
    
    gpio_isr_handler_add(BTN_MOVE_WAKE_GPIO, button_isr_handler, (void*) BTN_MOVE_WAKE_GPIO);
    gpio_isr_handler_add(BTN_AUDIO_WAKE_GPIO, button_isr_handler, (void*) BTN_AUDIO_WAKE_GPIO);
    gpio_isr_handler_add(BTN_BOOT_GPIO, button_isr_handler, (void*) BTN_BOOT_GPIO);
}

void dog_peripherals_init(void) {
    ESP_LOGI(TAG, "Initializing Dog Peripherals");
    init_buttons();
    init_mic();
    init_audio();
    init_display();
}

#endif
