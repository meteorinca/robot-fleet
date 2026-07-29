#include "dog_peripherals.h"
#include "config.h"

#include "paulbot_audio_8bit.h"
#include "dogbark_audio_8bit.h"
#include "extra_sounds.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

static const char *TAG = "SPEAKER_PERIPH";

// ── Audio PDM & Amp Handles ───────────────────────────────────────────────────
static i2s_chan_handle_t tx_chan = NULL;
static RingbufHandle_t audio_rb = NULL;

typedef struct {
    uint8_t *data;
    size_t size;
} audio_payload_t;

static QueueHandle_t audio_payload_queue = NULL;

static void dog_audio_task(void *arg) {
    size_t w_bytes = 0;
    static const int16_t silence[512] = {0}; // 1024 bytes of silence

    
    uint8_t leftover_byte = 0;
    bool has_leftover = false;
    bool stream_active = false;
    bool amp_enabled = false;

    while (1) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(audio_rb, &item_size, stream_active ? pdMS_TO_TICKS(150) : portMAX_DELAY);
        
        if (item) {
            if (!amp_enabled) {
#ifdef AUDIO_AMP_GPIO
                gpio_set_level(AUDIO_AMP_GPIO, 1);
#endif
                amp_enabled = true;
                vTaskDelay(pdMS_TO_TICKS(100)); // Give amp time to stabilize
                for (int i = 0; i < 4; i++) {
                    i2s_channel_write(tx_chan, silence, sizeof(silence), &w_bytes, portMAX_DELAY);
                }
            }
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
            if (stream_active) {
                for (int i = 0; i < 4; i++) {
                    i2s_channel_write(tx_chan, silence, sizeof(silence), &w_bytes, portMAX_DELAY);
                }
                stream_active = false;
                has_leftover = false;
                
#ifdef AUDIO_AMP_GPIO
                gpio_set_level(AUDIO_AMP_GPIO, 0);
#endif
                amp_enabled = false;
                ESP_LOGI(TAG, "Audio stream finished, Amp OFF.");
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
            ESP_LOGW(TAG, "Audio RB full, retrying...");
        }
    }
}

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
        ESP_LOGW(TAG, "Audio payload queue full");
        free(data);
    }
}

void dog_audio_play_8bit(const uint8_t *data, size_t len) {
    if (!audio_rb || !data || len == 0) return;
    
    const size_t chunk_samples = 1024;
    uint8_t *upsampled = malloc(chunk_samples * 2);
    if (!upsampled) return;

    size_t processed = 0;
    while (processed < len) {
        size_t to_process = len - processed;
        if (to_process > chunk_samples) to_process = chunk_samples;

        for (size_t i = 0; i < to_process; i++) {
            int8_t sample8 = (int8_t)data[processed + i];
            int16_t sample16 = sample8 << 8;
            upsampled[i*2] = sample16 & 0xFF;
            upsampled[i*2 + 1] = (sample16 >> 8) & 0xFF;
        }

        dog_audio_play_chunk(upsampled, to_process * 2);
        processed += to_process;
    }

    free(upsampled);
}

void dog_audio_play_paulbot(void) {
    ESP_LOGI(TAG, "Playing PaulBot audio...");
    dog_audio_play_8bit(paulbot_audio_8bit, paulbot_audio_8bit_len);
}

void dog_audio_play_bark(void) {
    ESP_LOGI(TAG, "Playing bark...");
    dog_audio_play_8bit(dogbark_audio_8bit, dogbark_audio_8bit_len);
}

void dog_audio_play_random(void) {
    if (random_sounds_count == 0) return;
    static int last_idx = -1;
    int idx = esp_random() % random_sounds_count;
    if (random_sounds_count > 1) {
        while (idx == last_idx) {
            idx = esp_random() % random_sounds_count;
        }
    }
    last_idx = idx;
    ESP_LOGI(TAG, "Playing random sound %d", idx);
    dog_audio_play_8bit(random_sounds[idx].data, random_sounds[idx].len);
}

void dog_audio_play_named(const char *name) {
    if (strcmp(name, "huh") == 0) dog_audio_play_8bit(sound_freesound_community_huh_102688, sound_freesound_community_huh_102688_len);
    else if (strcmp(name, "yes") == 0) dog_audio_play_8bit(sound_sergequadrado_child_says_yes_113117, sound_sergequadrado_child_says_yes_113117_len);
    else if (strcmp(name, "jump") == 0) dog_audio_play_8bit(sound_freesound_community_cartoon_jump_6462, sound_freesound_community_cartoon_jump_6462_len);
    else if (strcmp(name, "ding") == 0) dog_audio_play_8bit(sound_alexis_gaming_cam_ding_cartoon_346093, sound_alexis_gaming_cam_ding_cartoon_346093_len);
    else if (strcmp(name, "bark") == 0) dog_audio_play_bark();
    else if (strcmp(name, "paulbot") == 0) dog_audio_play_paulbot();
    else if (strcmp(name, "random") == 0) dog_audio_play_random();
    else ESP_LOGW(TAG, "Unknown sound name: %s", name);
}

void speaker_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0 || duration_ms == 0) return;
    uint32_t sample_rate = 16000;
    size_t total_samples = (sample_rate * duration_ms) / 1000;
    if (total_samples == 0) return;

    int16_t *buf = malloc(total_samples * sizeof(int16_t));
    if (!buf) return;

    float omega = 2.0f * M_PI * (float)freq_hz / (float)sample_rate;
    for (size_t i = 0; i < total_samples; i++) {
        buf[i] = (int16_t)(sinf(omega * i) * 12000.0f);
    }

    dog_audio_play_async((uint8_t *)buf, total_samples * sizeof(int16_t));
}

void speaker_play_drum_beat(void) {
    uint32_t sample_rate = 16000;
    uint32_t duration_ms = 90;
    size_t total_samples = (sample_rate * duration_ms) / 1000;
    int16_t *buf = malloc(total_samples * sizeof(int16_t));
    if (!buf) return;

    float phase = 0.0f;
    for (size_t i = 0; i < total_samples; i++) {
        float progress = (float)i / (float)total_samples;
        float freq = 160.0f * (1.0f - progress * 0.7f);
        float amp = 14000.0f * (1.0f - progress);
        phase += 2.0f * M_PI * freq / (float)sample_rate;
        buf[i] = (int16_t)(sinf(phase) * amp);
    }

    dog_audio_play_async((uint8_t *)buf, total_samples * sizeof(int16_t));
}

void dog_peripherals_init(void) {
    ESP_LOGI(TAG, "Initializing SpeakerBot MAX98357A I2S Audio Driver");
#ifdef AUDIO_AMP_GPIO
    gpio_config_t amp_conf = {
        .pin_bit_mask = (1ULL << AUDIO_AMP_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp_conf);
    gpio_set_level(AUDIO_AMP_GPIO, 0);
#endif

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK_GPIO,
            .ws   = AUDIO_LRCK_GPIO,
            .dout = AUDIO_DATA_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));


    audio_rb = xRingbufferCreate(16384, RINGBUF_TYPE_BYTEBUF);
    audio_payload_queue = xQueueCreate(16, sizeof(audio_payload_t));

    xTaskCreate(dog_audio_task, "spk_audio", 4096, NULL, 5, NULL);
    xTaskCreate(dog_audio_feeder_task, "spk_feed", 3072, NULL, 4, NULL);
}
