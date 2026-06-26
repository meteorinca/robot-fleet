#include "oled.h"
#include "board_config.h"

#ifdef OLED_SDA_PIN

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "font5x7.h"
#include "wifi_mgr.h"

static const char *TAG = "OLED";

#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

static uint8_t s_buffer[OLED_WIDTH * OLED_HEIGHT / 8];
static char s_oled_text_msg[64] = {0};
static volatile int s_oled_text_timer = 0;

void oled_set_text(const char* msg, int duration_ms) {
    strncpy(s_oled_text_msg, msg, sizeof(s_oled_text_msg) - 1);
    s_oled_text_timer = duration_ms / 60; // ~60ms per frame
}

static void oled_send_cmd(uint8_t cmd) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x00, true);
    i2c_master_write_byte(h, cmd, true);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_MASTER_NUM, h, portMAX_DELAY);
    i2c_cmd_link_delete(h);
}

static void oled_send_buffer(void) {
    for (int page = 0; page < 8; page++) {
        i2c_cmd_handle_t h = i2c_cmd_link_create();
        i2c_master_start(h);
        i2c_master_write_byte(h, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(h, 0x00, true);
        i2c_master_write_byte(h, 0xB0 + page, true);
        i2c_master_write_byte(h, 0x00, true);
        i2c_master_write_byte(h, 0x10, true);
        i2c_master_stop(h);
        i2c_master_cmd_begin(I2C_MASTER_NUM, h, portMAX_DELAY);
        i2c_cmd_link_delete(h);

        h = i2c_cmd_link_create();
        i2c_master_start(h);
        i2c_master_write_byte(h, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(h, 0x40, true);
        i2c_master_write(h, &s_buffer[page * OLED_WIDTH], OLED_WIDTH, true);
        i2c_master_stop(h);
        i2c_master_cmd_begin(I2C_MASTER_NUM, h, portMAX_DELAY);
        i2c_cmd_link_delete(h);
    }
}

static void draw_pixel(int x, int y, int color) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    if (color) s_buffer[(y / 8) * OLED_WIDTH + x] |= (1 << (y % 8));
    else       s_buffer[(y / 8) * OLED_WIDTH + x] &= ~(1 << (y % 8));
}

static void draw_text(int start_x, int start_y, const char* text, int scale) {
    int fw = 5 * scale;
    int char_space = 1 * scale;
    int len = strlen(text);
    
    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (c < 32 || c > 126) c = 32;
        const uint8_t *glyph = &font5x7[(c - 32) * 8];
        int cx = start_x + i * (fw + char_space);
        for (int gx = 0; gx < 5; gx++) {
            uint8_t col = glyph[gx];
            for (int gy = 0; gy < 7; gy++) {
                if (col & (1 << gy)) {
                    for (int dx = 0; dx < scale; dx++) {
                        for (int dy = 0; dy < scale; dy++) {
                            draw_pixel(cx + gx * scale + dx, start_y + gy * scale + dy, 1);
                        }
                    }
                }
            }
        }
    }
}

static void oled_eyes_task(void *arg) {
    int blink_timer = 0;
    int next_blink = 50 + (esp_random() % 100);
    bool blinking = false;
    int frame_count = 0;

    static int pupil_dx = 0;
    static int pupil_dy = 0;
    static int pupil_target_dx = 0;
    static int pupil_target_dy = 0;

    const int eye_cx[2] = { 32, 96 };
    const int eye_cy = 32;
    const int eye_rx = 26;
    const int base_ry = 26;
    const int base_pupil_r = 8;

    while (1) {
        frame_count++;
        
        memset(s_buffer, 0, sizeof(s_buffer));

        if (s_oled_text_timer > 0) {
            s_oled_text_timer--;
            
            int msg_len = strlen(s_oled_text_msg);
            int scale = (msg_len == 0) ? 2 : (OLED_WIDTH / (6 * msg_len - 1));
            if (scale > 2) scale = 2;
            if (scale < 1) scale = 1;
            int fw = 5 * scale;
            int fh = 7 * scale;
            int char_space = 1 * scale;
            int total_w = msg_len * (fw + char_space) - char_space;
            int start_x = (OLED_WIDTH - total_w) / 2;
            int start_y = (OLED_HEIGHT - fh) / 2;
            draw_text(start_x, start_y, s_oled_text_msg, scale);
            
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }

        // Check WiFi status
        wifi_state_t wstate = wifi_mgr_get_state();
        if (wstate == WIFI_STATE_AP_MODE) {
            draw_text(0, 0, "AP MODE", 2);
            draw_text(0, 20, "192.168.4.1", 1);
            draw_text(0, 40, "Connect to MyBot", 1);
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else if (wstate == WIFI_STATE_CONNECTING) {
            draw_text(10, 24, "Connecting...", 1);
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else if (wstate == WIFI_STATE_CONNECTED) {
            static int show_ip_timer = 0;
            if (show_ip_timer < 50) { // ~3 seconds at 60ms/frame
                show_ip_timer++;
                draw_text(10, 10, "Connected!", 1);
                char ip_buf[32];
                snprintf(ip_buf, sizeof(ip_buf), "%s", wifi_mgr_get_ip());
                draw_text(10, 30, ip_buf, 2);
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(60));
                continue;
            }
            // fallthrough to eyes
        }

        // Animated eyes
        blink_timer++;
        if (!blinking && blink_timer > next_blink) {
            blinking = true;
            blink_timer = 0;
        }

        int max_ry = base_ry;
        if (blinking) {
            max_ry = 3;
            if (blink_timer > 1) {
                blinking = false;
                blink_timer = 0;
                next_blink = 40 + (esp_random() % 100);
            }
        }

        if (esp_random() % 20 == 0) {
            pupil_target_dx = (esp_random() % 14) - 7;
            pupil_target_dy = (esp_random() % 8) - 4;
        }
        if (pupil_dx < pupil_target_dx) pupil_dx++;
        if (pupil_dx > pupil_target_dx) pupil_dx--;
        if (pupil_dy < pupil_target_dy) pupil_dy++;
        if (pupil_dy > pupil_target_dy) pupil_dy--;

        int pupil_r = base_pupil_r + ((frame_count / 12) % 3) - 1;
        int pupil_r_sq = pupil_r * pupil_r;
        int rx_sq = eye_rx * eye_rx;
        int ry_sq = max_ry * max_ry;
        int ellipse_limit = rx_sq * ry_sq;

        for (int y = 0; y < OLED_HEIGHT; y++) {
            int dy = y - eye_cy;
            int dy_sq_rx = dy * dy * rx_sq;

            for (int x = 0; x < OLED_WIDTH; x++) {
                int ei_start, ei_end;
                if (x < 6) { ei_start = 2; ei_end = 2; }
                else if (x < 60) { ei_start = 0; ei_end = 1; }
                else if (x < 68) { ei_start = 2; ei_end = 2; }
                else if (x < 122) { ei_start = 1; ei_end = 2; }
                else { ei_start = 2; ei_end = 2; }

                for (int ei = ei_start; ei < ei_end; ei++) {
                    int dx = x - eye_cx[ei];
                    int ex_val = dx * dx * ry_sq + dy_sq_rx;
                    if (ex_val > ellipse_limit) continue;

                    int idx = dx - pupil_dx;
                    int idy = dy - pupil_dy;
                    if (idx * idx + idy * idy <= pupil_r_sq) {
                        // Pupil (black)
                    } else {
                        // Sclera (white)
                        draw_pixel(x, y, 1);
                    }
                    break;
                }
            }
        }

        oled_send_buffer();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

void oled_init(void) {
    ESP_LOGI(TAG, "Initializing I2C OLED (SDA=%d, SCL=%d)", OLED_SDA_PIN, OLED_SCL_PIN);
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_SDA_PIN,
        .scl_io_num = OLED_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    // SSD1306 Init Sequence
    vTaskDelay(pdMS_TO_TICKS(100));
    oled_send_cmd(0xAE); // display off
    oled_send_cmd(0x20); // Set Memory Addressing Mode
    oled_send_cmd(0x00); // 00,Horizontal Addressing Mode
    oled_send_cmd(0x40); // set start line address
    oled_send_cmd(0xA1); // set segment re-map 0 to 127
    oled_send_cmd(0xA8); // set multiplex ratio(1 to 64)
    oled_send_cmd(0x3F); //
    oled_send_cmd(0xC8); // Set COM Output Scan Direction
    oled_send_cmd(0xD3); // set display offset
    oled_send_cmd(0x00); // not offset
    oled_send_cmd(0xDA); // set com pins hardware configuration
    oled_send_cmd(0x12);
    oled_send_cmd(0x81); // set contrast control register
    oled_send_cmd(0xCF);
    oled_send_cmd(0xD9); // set pre-charge period
    oled_send_cmd(0xF1);
    oled_send_cmd(0xDB); // set vcomh
    oled_send_cmd(0x40);
    oled_send_cmd(0xA4); // Output RAM to Display
    oled_send_cmd(0xA6); // set normal display
    oled_send_cmd(0xAF); // display on

    xTaskCreate(oled_eyes_task, "oled_eyes", 4096, NULL, 1, NULL);
}

#endif
