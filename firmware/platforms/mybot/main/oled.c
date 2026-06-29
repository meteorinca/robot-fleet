#include "oled.h"
#include "board_config.h"

#ifdef OLED_SDA_PIN

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
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

static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t dev_handle;

static uint8_t s_buffer[OLED_WIDTH * OLED_HEIGHT / 8];
static char s_oled_text_msg[64] = {0};
static volatile int s_oled_text_timer = 0;

static volatile oled_mode_t s_oled_mode = OLED_MODE_NORMAL;
static volatile eye_emotion_t s_eye_emotion = EYE_EMOTION_NORMAL;
static volatile bool s_paddle_left = false;
static volatile bool s_paddle_right = false;

void oled_set_mode(oled_mode_t mode) { s_oled_mode = mode; }
oled_mode_t oled_get_mode(void) { return s_oled_mode; }
void oled_set_emotion(eye_emotion_t emotion) { s_eye_emotion = emotion; }
eye_emotion_t oled_get_emotion(void) { return s_eye_emotion; }
void oled_set_paddle_input(bool left, bool right) { s_paddle_left = left; s_paddle_right = right; }


void oled_set_text(const char* msg, int duration_ms) {
    strncpy(s_oled_text_msg, msg, sizeof(s_oled_text_msg) - 1);
    s_oled_text_timer = duration_ms / 60; // ~60ms per frame
}

static void oled_send_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    i2c_master_transmit(dev_handle, buf, sizeof(buf), -1);
}

static void oled_send_buffer(void) {
    for (int page = 0; page < 8; page++) {
        oled_send_cmd(0xB0 + page);
        oled_send_cmd(0x00);
        oled_send_cmd(0x10);

        uint8_t buf[OLED_WIDTH + 1];
        buf[0] = 0x40;
        memcpy(buf + 1, &s_buffer[page * OLED_WIDTH], OLED_WIDTH);
        i2c_master_transmit(dev_handle, buf, sizeof(buf), -1);
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

        if (s_oled_mode == OLED_MODE_PONG_V || s_oled_mode == OLED_MODE_PONG_H) {
            static float ball_x = OLED_WIDTH / 2;
            static float ball_y = OLED_HEIGHT / 2;
            static float ball_dx = 2.0f;
            static float ball_dy = 2.0f;
            static float paddle_player_pos = OLED_WIDTH / 2;
            static float paddle_ai_pos = OLED_WIDTH / 2;
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            
            bool is_h = (s_oled_mode == OLED_MODE_PONG_H);
            
            const int paddle_len = 20;
            const int paddle_thick = 4;
            const int ball_size = 4;
            
            int max_pos = is_h ? OLED_HEIGHT : OLED_WIDTH;
            
            if (last_mode != s_oled_mode) {
                ball_x = OLED_WIDTH / 2;
                ball_y = OLED_HEIGHT / 2;
                paddle_player_pos = max_pos / 2;
                paddle_ai_pos = max_pos / 2;
                last_mode = s_oled_mode;
            }

            // Move player
            if (s_paddle_left) paddle_player_pos -= 3.0f;
            if (s_paddle_right) paddle_player_pos += 3.0f;
            if (paddle_player_pos < paddle_len/2) paddle_player_pos = paddle_len/2;
            if (paddle_player_pos > max_pos - paddle_len/2) paddle_player_pos = max_pos - paddle_len/2;
            
            // Move AI
            float ball_pos_for_ai = is_h ? ball_y : ball_x;
            if (ball_pos_for_ai < paddle_ai_pos - 4) paddle_ai_pos -= 1.5f;
            else if (ball_pos_for_ai > paddle_ai_pos + 4) paddle_ai_pos += 1.5f;
            if (paddle_ai_pos < paddle_len/2) paddle_ai_pos = paddle_len/2;
            if (paddle_ai_pos > max_pos - paddle_len/2) paddle_ai_pos = max_pos - paddle_len/2;
            
            // Move ball
            ball_x += ball_dx;
            ball_y += ball_dy;
            
            if (is_h) {
                // Horizontal pong (paddles left/right)
                // Bounce top/bottom walls
                if (ball_y < 0) { ball_y = 0; ball_dy = -ball_dy; }
                if (ball_y > OLED_HEIGHT - ball_size) { ball_y = OLED_HEIGHT - ball_size; ball_dy = -ball_dy; }
                
                // Player paddle is on the Right side
                int px = OLED_WIDTH - paddle_thick - 2;
                if (ball_x + ball_size >= px && ball_x <= px + paddle_thick) {
                    if (ball_y + ball_size >= paddle_player_pos - paddle_len/2 && ball_y <= paddle_player_pos + paddle_len/2) {
                        ball_x = px - ball_size;
                        ball_dx = -ball_dx;
                        ball_dy = (ball_y - paddle_player_pos) * 0.2f;
                    }
                }
                // AI paddle is on the Left side
                int ax = 2;
                if (ball_x <= ax + paddle_thick && ball_x + ball_size >= ax) {
                    if (ball_y + ball_size >= paddle_ai_pos - paddle_len/2 && ball_y <= paddle_ai_pos + paddle_len/2) {
                        ball_x = ax + paddle_thick;
                        ball_dx = -ball_dx;
                    }
                }
                
                // Score
                if (ball_x < 0 || ball_x > OLED_WIDTH) {
                    ball_x = OLED_WIDTH / 2;
                    ball_y = OLED_HEIGHT / 2;
                    ball_dx = -ball_dx;
                }
                
                // Draw paddles
                for (int i=0; i<paddle_thick; i++) {
                    for (int j=0; j<paddle_len; j++) {
                        draw_pixel(px + i, (int)paddle_player_pos - paddle_len/2 + j, 1);
                        draw_pixel(ax + i, (int)paddle_ai_pos - paddle_len/2 + j, 1);
                    }
                }
            } else {
                // Vertical pong (paddles top/bottom)
                // Bounce left/right walls
                if (ball_x < 0) { ball_x = 0; ball_dx = -ball_dx; }
                if (ball_x > OLED_WIDTH - ball_size) { ball_x = OLED_WIDTH - ball_size; ball_dx = -ball_dx; }
                
                // Player paddle at bottom
                int py = OLED_HEIGHT - paddle_thick - 2;
                if (ball_y + ball_size >= py && ball_y <= py + paddle_thick) {
                    if (ball_x + ball_size >= paddle_player_pos - paddle_len/2 && ball_x <= paddle_player_pos + paddle_len/2) {
                        ball_y = py - ball_size;
                        ball_dy = -ball_dy;
                        ball_dx = (ball_x - paddle_player_pos) * 0.2f;
                    }
                }
                // AI paddle at top
                int ay = 2;
                if (ball_y <= ay + paddle_thick && ball_y + ball_size >= ay) {
                    if (ball_x + ball_size >= paddle_ai_pos - paddle_len/2 && ball_x <= paddle_ai_pos + paddle_len/2) {
                        ball_y = ay + paddle_thick;
                        ball_dy = -ball_dy;
                    }
                }
                
                // Score
                if (ball_y < 0 || ball_y > OLED_HEIGHT) {
                    ball_x = OLED_WIDTH / 2;
                    ball_y = OLED_HEIGHT / 2;
                    ball_dy = -ball_dy;
                }
                
                // Draw paddles
                for (int i=0; i<paddle_len; i++) {
                    for (int j=0; j<paddle_thick; j++) {
                        draw_pixel((int)paddle_player_pos - paddle_len/2 + i, py + j, 1);
                        draw_pixel((int)paddle_ai_pos - paddle_len/2 + i, ay + j, 1);
                    }
                }
            }
            
            // Draw ball
            for (int i=0; i<ball_size; i++) {
                for (int j=0; j<ball_size; j++) {
                    draw_pixel((int)ball_x + i, (int)ball_y + j, 1);
                }
            }
            
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(30)); // 30ms for faster pong
            continue;
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
                        bool draw_it = true;
                        if (s_eye_emotion == EYE_EMOTION_MAD) {
                            if (dy < ((ei == 0) ? dx : -dx) / 2 - 4) draw_it = false;
                        } else if (s_eye_emotion == EYE_EMOTION_SAD) {
                            if (dy < ((ei == 0) ? -dx : dx) / 2 - 4) draw_it = false;
                        } else if (s_eye_emotion == EYE_EMOTION_SLEEPY) {
                            if (dy < 6) draw_it = false;
                        }
                        
                        if (draw_it) draw_pixel(x, y, 1);
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
    
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = OLED_SCL_PIN,
        .sda_io_num = OLED_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle));

    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    for (int i = 0; i < sizeof(init_cmds); i++) {
        oled_send_cmd(init_cmds[i]);
    }

    xTaskCreate(oled_eyes_task, "oled_eyes", 4096, NULL, 1, NULL);
}

#endif
