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
#include <math.h>

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

static void draw_sprite8(int start_x, int start_y, const uint8_t *sprite, int width, int height) {
    for (int y = 0; y < height; y++) {
        uint8_t row = sprite[y];
        for (int x = 0; x < width; x++) {
            if (row & (1 << (7 - x))) {
                draw_pixel(start_x + x, start_y + y, 1);
            }
        }
    }
}

static const uint8_t b_flap[8] = { 0x1C, 0x3E, 0x76, 0xFF, 0x7E, 0x3C, 0x00, 0x00 };
static const uint8_t b_fall[8] = { 0x1C, 0x3E, 0x76, 0x7F, 0xFE, 0x3C, 0x00, 0x00 };
static const uint8_t d_run1[8] = { 0x0E, 0x0F, 0x0C, 0x3C, 0x7C, 0x7C, 0x10, 0x40 };
static const uint8_t d_run2[8] = { 0x0E, 0x0F, 0x0C, 0x3C, 0x7C, 0x7C, 0x40, 0x10 };
static const uint8_t c_cactus[10]= { 0x18, 0x58, 0x5A, 0x7A, 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18 };

static void draw_line(int x0, int y0, int x1, int y1, int color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int max_iters = 1000;
    while (max_iters--) {
        if (x0 >= 0 && x0 < OLED_WIDTH && y0 >= 0 && y0 < OLED_HEIGHT) {
            draw_pixel(x0, y0, color);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

typedef struct { float x, y, z; } vec3_t;

static vec3_t rotate_x(vec3_t v, float angle) {
    float s = sinf(angle), c = cosf(angle);
    return (vec3_t){ v.x, v.y * c - v.z * s, v.y * s + v.z * c };
}
static vec3_t rotate_y(vec3_t v, float angle) {
    float s = sinf(angle), c = cosf(angle);
    return (vec3_t){ v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}
static vec3_t rotate_z(vec3_t v, float angle) {
    float s = sinf(angle), c = cosf(angle);
    return (vec3_t){ v.x * c - v.y * s, v.x * s + v.y * c, v.z };
}

static bool project_3d(vec3_t v, int *px, int *py) {
    float focal_len = 80.0f;
    float z_offset = 50.0f;
    v.z += z_offset; 
    if (v.z <= 0.1f) return false; 
    *px = (int)(OLED_WIDTH/2 + (v.x * focal_len) / v.z);
    *py = (int)(OLED_HEIGHT/2 - (v.y * focal_len) / v.z);
    return true;
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
            static int score_p = 0, score_ai = 0;
            
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
                score_p = 0; score_ai = 0;
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
                if (ball_y < 0) { ball_y = 0; ball_dy = -ball_dy; }
                if (ball_y > OLED_HEIGHT - ball_size) { ball_y = OLED_HEIGHT - ball_size; ball_dy = -ball_dy; }
                
                int px = OLED_WIDTH - paddle_thick - 2;
                if (ball_x + ball_size >= px && ball_x <= px + paddle_thick) {
                    if (ball_y + ball_size >= paddle_player_pos - paddle_len/2 && ball_y <= paddle_player_pos + paddle_len/2) {
                        ball_x = px - ball_size;
                        ball_dx = -ball_dx;
                        ball_dy = (ball_y - paddle_player_pos) * 0.2f;
                    }
                }
                int ax = 2;
                if (ball_x <= ax + paddle_thick && ball_x + ball_size >= ax) {
                    if (ball_y + ball_size >= paddle_ai_pos - paddle_len/2 && ball_y <= paddle_ai_pos + paddle_len/2) {
                        ball_x = ax + paddle_thick;
                        ball_dx = -ball_dx;
                    }
                }
                
                // Score
                if (ball_x < 0) { score_p++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dx = 2.0f; }
                if (ball_x > OLED_WIDTH) { score_ai++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dx = -2.0f; }
                
                // Center Line
                for (int y = 0; y < OLED_HEIGHT; y += 4) draw_pixel(OLED_WIDTH/2, y, 1);
                // Scores
                char buf[16]; snprintf(buf, sizeof(buf), "%d", score_ai);
                draw_text(OLED_WIDTH/2 - 20, 2, buf, 1);
                snprintf(buf, sizeof(buf), "%d", score_p);
                draw_text(OLED_WIDTH/2 + 10, 2, buf, 1);

                // Draw paddles
                for (int i=0; i<paddle_thick; i++) {
                    for (int j=0; j<paddle_len; j++) {
                        draw_pixel(px + i, (int)paddle_player_pos - paddle_len/2 + j, 1);
                        draw_pixel(ax + i, (int)paddle_ai_pos - paddle_len/2 + j, 1);
                    }
                }
            } else {
                // Vertical pong (paddles top/bottom)
                if (ball_x < 0) { ball_x = 0; ball_dx = -ball_dx; }
                if (ball_x > OLED_WIDTH - ball_size) { ball_x = OLED_WIDTH - ball_size; ball_dx = -ball_dx; }
                
                int py = OLED_HEIGHT - paddle_thick - 2;
                if (ball_y + ball_size >= py && ball_y <= py + paddle_thick) {
                    if (ball_x + ball_size >= paddle_player_pos - paddle_len/2 && ball_x <= paddle_player_pos + paddle_len/2) {
                        ball_y = py - ball_size;
                        ball_dy = -ball_dy;
                        ball_dx = (ball_x - paddle_player_pos) * 0.2f;
                    }
                }
                int ay = 2;
                if (ball_y <= ay + paddle_thick && ball_y + ball_size >= ay) {
                    if (ball_x + ball_size >= paddle_ai_pos - paddle_len/2 && ball_x <= paddle_ai_pos + paddle_len/2) {
                        ball_y = ay + paddle_thick;
                        ball_dy = -ball_dy;
                    }
                }
                
                // Score
                if (ball_y < 0) { score_p++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dy = 2.0f; }
                if (ball_y > OLED_HEIGHT) { score_ai++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dy = -2.0f; }
                
                // Center Line
                for (int x = 0; x < OLED_WIDTH; x += 4) draw_pixel(x, OLED_HEIGHT/2, 1);
                // Scores
                char buf[16]; snprintf(buf, sizeof(buf), "%d", score_ai);
                draw_text(2, OLED_HEIGHT/2 - 12, buf, 1);
                snprintf(buf, sizeof(buf), "%d", score_p);
                draw_text(2, OLED_HEIGHT/2 + 6, buf, 1);

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
        } else if (s_oled_mode == OLED_MODE_FLAPPY) {
            static float bird_y = 32;
            static float bird_dy = 0;
            static float pipe_x = 128;
            static float cloud_x1 = 0, cloud_x2 = 64;
            static int pipe_gap_y = 32;
            static int score = 0;
            static bool game_over = false;
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            static bool p_btn = false;
            
            if (last_mode != s_oled_mode) {
                bird_y = 32; bird_dy = 0; pipe_x = 128; pipe_gap_y = 32; score = 0; game_over = false;
                last_mode = s_oled_mode;
            }

            if (game_over) {
                draw_text(20, 20, "GAME OVER", 2);
                char sb[32]; snprintf(sb, sizeof(sb), "Score: %d", score);
                draw_text(30, 40, sb, 1);
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(100));
                bool btn = s_paddle_left || s_paddle_right;
                if (btn && !p_btn) {
                    bird_y = 32; bird_dy = 0; pipe_x = 128; pipe_gap_y = 32; score = 0; game_over = false;
                }
                p_btn = btn;
                continue;
            }

            bool btn = s_paddle_left || s_paddle_right;
            if (btn && !p_btn) {
                bird_dy = -3.5f;
            }
            p_btn = btn;
            
            bird_dy += 0.3f;
            bird_y += bird_dy;
            if (bird_y < 0) { bird_y = 0; bird_dy = 0; }
            if (bird_y > 63) { game_over = true; }
            
            pipe_x -= 3.0f;
            if (pipe_x < -14) {
                pipe_x = 128;
                pipe_gap_y = 15 + (esp_random() % 34);
                score++;
            }
            
            int bx = 20, by = (int)bird_y;
            int pw = 14, pg = 20;
            if (bx + 8 >= pipe_x && bx <= pipe_x + pw) {
                if (by <= pipe_gap_y - pg || by + 6 >= pipe_gap_y + pg) {
                    game_over = true;
                }
            }
            
            // Background clouds
            cloud_x1 -= 0.5f; cloud_x2 -= 0.2f;
            if (cloud_x1 < -20) cloud_x1 = 128;
            if (cloud_x2 < -20) cloud_x2 = 128;
            draw_text((int)cloud_x1, 8, "~ ~", 1);
            draw_text((int)cloud_x2, 28, "~", 1);
            
            // Draw Bird Sprite
            const uint8_t* bird_sprite = (bird_dy < 0) ? b_flap : b_fall;
            draw_sprite8(bx, by, bird_sprite, 8, 6);
            
            // Draw Pipe with details
            for (int x=0; x<pw; x++) {
                if (pipe_x + x >= 0 && pipe_x + x < 128) {
                    for (int y=0; y<64; y++) {
                        if (y < pipe_gap_y - pg || y > pipe_gap_y + pg) {
                            if (x == 0 || x == pw-1) draw_pixel((int)pipe_x + x, y, 1);
                            else if ((x + y) % 4 != 0) draw_pixel((int)pipe_x + x, y, 1);
                        }
                    }
                }
            }
            // Pipe end caps
            for(int x=-2; x<pw+2; x++) {
                for(int y=0; y<3; y++) {
                    draw_pixel((int)pipe_x + x, pipe_gap_y - pg - y, 1);
                    draw_pixel((int)pipe_x + x, pipe_gap_y + pg + y, 1);
                }
            }
            
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score);
            draw_text(2, 2, sb, 1);
            
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_DINO) {
            static float dino_y = 50;
            static float dino_dy = 0;
            static float cactus_x = 128;
            static float cloud_x1 = 30;
            static int score = 0;
            static float speed = 3.0f;
            static bool game_over = false;
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            static bool p_btn = false;
            
            if (last_mode != s_oled_mode) {
                dino_y = 50; dino_dy = 0; cactus_x = 128; score = 0; speed = 3.0f; game_over = false;
                last_mode = s_oled_mode;
            }

            if (game_over) {
                draw_text(20, 20, "GAME OVER", 2);
                char sb[32]; snprintf(sb, sizeof(sb), "Score: %d", score);
                draw_text(30, 40, sb, 1);
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(100));
                bool btn = s_paddle_left || s_paddle_right;
                if (btn && !p_btn) {
                    dino_y = 50; dino_dy = 0; cactus_x = 128; score = 0; speed = 3.0f; game_over = false;
                }
                p_btn = btn;
                continue;
            }

            bool btn = s_paddle_left || s_paddle_right;
            if (btn && !p_btn && dino_y >= 50) {
                dino_dy = -5.0f;
            }
            p_btn = btn;
            
            dino_dy += 0.4f;
            dino_y += dino_dy;
            if (dino_y > 50) { dino_y = 50; dino_dy = 0; }
            
            cactus_x -= speed;
            if (cactus_x < -10) {
                cactus_x = 128 + (esp_random() % 50);
                score++;
                speed += 0.1f;
                if (speed > 8.0f) speed = 8.0f;
            }
            
            int dx = 20, dy = (int)dino_y;
            int cx = (int)cactus_x, cy = 48;
            if (dx + 6 >= cx && dx <= cx + 6) {
                if (dy + 8 >= cy) {
                    game_over = true;
                }
            }
            
            // Clouds
            cloud_x1 -= speed * 0.2f;
            if (cloud_x1 < -20) cloud_x1 = 128;
            draw_text((int)cloud_x1, 15, "===", 1);
            
            // Draw Ground Detail
            for (int x=0; x<128; x++) {
                draw_pixel(x, 58, 1);
                if ((x + (int)cactus_x/2) % 15 == 0) draw_pixel(x, 59, 1);
                if ((x + (int)cactus_x/3) % 23 == 0) draw_pixel(x, 60, 1);
            }
            
            // Draw Dino
            const uint8_t* dino_sprite = (dino_y < 50) ? d_run1 : ((frame_count / 3) % 2 == 0 ? d_run1 : d_run2);
            draw_sprite8(dx, dy, dino_sprite, 8, 8);
            
            // Draw Cactus
            if (cx >= -6 && cx < 128) {
                draw_sprite8(cx, cy, c_cactus, 8, 10);
            }
            
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score);
            draw_text(90, 2, sb, 1);
            
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_SNAKE) {
            static int snake_x[64];
            static int snake_y[64];
            static int snake_len = 3;
            static int snake_dir = 1;
            static int food_x = 20;
            static int food_y = 10;
            static bool game_over = false;
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            static bool p_left = false;
            static bool p_right = false;
            
            if (last_mode != s_oled_mode) {
                snake_len = 3; snake_dir = 1; game_over = false;
                for (int i=0; i<64; i++) { snake_x[i] = 10-i; snake_y[i] = 10; }
                food_x = 20 + (esp_random() % 10);
                food_y = 10 + (esp_random() % 5);
                last_mode = s_oled_mode;
            }

            if (game_over) {
                draw_text(20, 20, "GAME OVER", 2);
                char sb[32]; snprintf(sb, sizeof(sb), "Score: %d", snake_len-3);
                draw_text(30, 40, sb, 1);
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(100));
                if ((s_paddle_left && !p_left) || (s_paddle_right && !p_right)) {
                    snake_len = 3; snake_dir = 1; game_over = false;
                    for (int i=0; i<64; i++) { snake_x[i] = 10-i; snake_y[i] = 10; }
                }
                p_left = s_paddle_left; p_right = s_paddle_right;
                continue;
            }

            if (s_paddle_left && !p_left) snake_dir = (snake_dir + 3) % 4;
            if (s_paddle_right && !p_right) snake_dir = (snake_dir + 1) % 4;
            p_left = s_paddle_left; p_right = s_paddle_right;
            
            for (int i=snake_len-1; i>0; i--) {
                snake_x[i] = snake_x[i-1];
                snake_y[i] = snake_y[i-1];
            }
            
            if (snake_dir == 0) snake_y[0]--;
            else if (snake_dir == 1) snake_x[0]++;
            else if (snake_dir == 2) snake_y[0]++;
            else if (snake_dir == 3) snake_x[0]--;
            
            if (snake_x[0] < 0 || snake_x[0] >= 128/3 || snake_y[0] < 0 || snake_y[0] >= 64/3) {
                game_over = true;
            }
            
            for (int i=1; i<snake_len; i++) {
                if (snake_x[0] == snake_x[i] && snake_y[0] == snake_y[i]) game_over = true;
            }
            
            if (snake_x[0] == food_x && snake_y[0] == food_y) {
                if (snake_len < 64) snake_len++;
                food_x = esp_random() % (128/3);
                food_y = esp_random() % (64/3);
            }
            
            // Draw Diamond Food
            int fx = food_x * 3, fy = food_y * 3;
            draw_pixel(fx+1, fy, 1);
            draw_pixel(fx, fy+1, 1); draw_pixel(fx+2, fy+1, 1);
            draw_pixel(fx+1, fy+2, 1);
            
            // Draw Snake Detailed
            for (int k=0; k<snake_len; k++) {
                int px = snake_x[k] * 3, py = snake_y[k] * 3;
                if (k == 0) {
                    for (int i=0; i<3; i++) for (int j=0; j<3; j++) draw_pixel(px+i, py+j, 1);
                    if (snake_dir == 0) { draw_pixel(px, py+1, 0); draw_pixel(px+2, py+1, 0); }
                    else if (snake_dir == 1) { draw_pixel(px+1, py, 0); draw_pixel(px+1, py+2, 0); }
                    else if (snake_dir == 2) { draw_pixel(px, py+1, 0); draw_pixel(px+2, py+1, 0); }
                    else if (snake_dir == 3) { draw_pixel(px+1, py, 0); draw_pixel(px+1, py+2, 0); }
                } else {
                    draw_pixel(px+1, py, 1);
                    draw_pixel(px, py+1, 1); draw_pixel(px+2, py+1, 1);
                    draw_pixel(px+1, py+2, 1);
                }
            }
            
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        } else if (s_oled_mode == OLED_MODE_3D_SHOWCASE) {
            static int anim_idx = 0;
            static float angle_x = 0;
            static float angle_y = 0;
            static float angle_z = 0;
            static float t = 0;
            static bool p_left = false;
            static bool p_right = false;
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            
            if (last_mode != s_oled_mode) {
                anim_idx = 0; angle_x = 0; angle_y = 0; angle_z = 0; t = 0;
                last_mode = s_oled_mode;
            }
            
            if (s_paddle_left && !p_left) anim_idx = (anim_idx + 5) % 6;
            if (s_paddle_right && !p_right) anim_idx = (anim_idx + 1) % 6;
            p_left = s_paddle_left; p_right = s_paddle_right;
            
            angle_x += 0.05f; angle_y += 0.03f; angle_z += 0.02f; t += 0.1f;
            
            if (anim_idx == 0) { // Cube
                draw_text(0, 0, "Cube", 1);
                vec3_t pts[8] = {
                    {-10,-10,-10}, {10,-10,-10}, {10,10,-10}, {-10,10,-10},
                    {-10,-10,10}, {10,-10,10}, {10,10,10}, {-10,10,10}
                };
                int px[8], py[8];
                for(int i=0; i<8; i++) {
                    vec3_t v = rotate_x(rotate_y(rotate_z(pts[i], angle_z), angle_y), angle_x);
                    project_3d(v, &px[i], &py[i]);
                }
                int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
                for(int i=0; i<12; i++) {
                    draw_line(px[edges[i][0]], py[edges[i][0]], px[edges[i][1]], py[edges[i][1]], 1);
                }
            } else if (anim_idx == 1) { // DNA
                draw_text(0, 0, "DNA Helix", 1);
                for(int i=-20; i<=20; i+=2) {
                    float local_t = t + i * 0.2f;
                    vec3_t p1 = {sinf(local_t)*10.0f, (float)i, cosf(local_t)*10.0f};
                    vec3_t p2 = {sinf(local_t + 3.1415f)*10.0f, (float)i, cosf(local_t + 3.1415f)*10.0f};
                    p1 = rotate_x(p1, 0.5f); p2 = rotate_x(p2, 0.5f);
                    int x1, y1, x2, y2;
                    project_3d(p1, &x1, &y1); project_3d(p2, &x2, &y2);
                    draw_pixel(x1, y1, 1); draw_pixel(x2, y2, 1);
                    if (i % 4 == 0) draw_line(x1, y1, x2, y2, 1);
                }
            } else if (anim_idx == 2) { // Starfield
                draw_text(0, 0, "Starfield", 1);
                static vec3_t stars[50];
                static bool init = false;
                if (!init) {
                    for(int i=0; i<50; i++) {
                        stars[i] = (vec3_t){(float)((esp_random()%100)-50), (float)((esp_random()%100)-50), (float)(esp_random()%100)};
                    }
                    init = true;
                }
                for(int i=0; i<50; i++) {
                    stars[i].z -= 2.0f;
                    if (stars[i].z < 0) { stars[i] = (vec3_t){(float)((esp_random()%100)-50), (float)((esp_random()%100)-50), 100.0f}; }
                    int px, py, px_old, py_old;
                    vec3_t tail = stars[i]; tail.z += 4.0f;
                    if (project_3d(stars[i], &px, &py) && project_3d(tail, &px_old, &py_old)) {
                        draw_line(px, py, px_old, py_old, 1);
                    }
                }
            } else if (anim_idx == 3) { // Torus
                draw_text(0, 0, "Torus", 1);
                float R = 15.0f;
                float r = 5.0f;
                for(int i=0; i<12; i++) {
                    float theta = i * 3.14159f / 6.0f;
                    for(int j=0; j<8; j++) {
                        float phi = j * 3.14159f / 4.0f;
                        vec3_t p = { (R + r * cosf(phi)) * cosf(theta), (R + r * cosf(phi)) * sinf(theta), r * sinf(phi) };
                        p = rotate_x(rotate_y(p, angle_y), angle_x);
                        int px, py;
                        if (project_3d(p, &px, &py)) draw_pixel(px, py, 1);
                    }
                }
            } else if (anim_idx == 4) { // Wave
                draw_text(0, 0, "Wave Grid", 1);
                for(int x=-20; x<=20; x+=5) {
                    for(int z=-20; z<=20; z+=5) {
                        float y = sinf((x)*0.2f + t) * 5.0f + cosf((z)*0.2f + t) * 5.0f;
                        vec3_t p = rotate_x(rotate_y((vec3_t){(float)x, y, (float)z}, angle_y), 0.5f);
                        int px, py;
                        if (project_3d(p, &px, &py)) draw_pixel(px, py, 1);
                    }
                }
            } else if (anim_idx == 5) { // Spirograph
                draw_text(0, 0, "Spirograph", 1);
                for(float i=0; i<6.28f; i+=0.1f) {
                    float r = 10.0f * sinf(4.0f * i + t);
                    vec3_t p = { r * cosf(i), r * sinf(i), 5.0f * sinf(i * 3.0f + t) };
                    p = rotate_x(rotate_y(p, angle_y), angle_x);
                    int px, py;
                    if (project_3d(p, &px, &py)) draw_pixel(px, py, 1);
                }
            }
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_PACMAN) {
            static int px = 64, py = 32;
            static int gx = 20, gy = 20;
            static int score = 0;
            static int dots_x[20], dots_y[20];
            static bool dots_active[20];
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            static bool p_left=false, p_right=false;
            static int dir = 0;
            
            if (last_mode != s_oled_mode) {
                px = 64; py = 32; gx = 20; gy = 20; score = 0; dir = 1;
                for(int i=0; i<20; i++) { dots_x[i] = esp_random()%120; dots_y[i] = esp_random()%60; dots_active[i] = true; }
                last_mode = s_oled_mode;
            }
            if (s_paddle_left && !p_left) dir = (dir + 3) % 4;
            if (s_paddle_right && !p_right) dir = (dir + 1) % 4;
            p_left = s_paddle_left; p_right = s_paddle_right;
            
            if (dir == 0) py-=2; else if (dir == 1) px+=2; else if (dir == 2) py+=2; else if (dir == 3) px-=2;
            if (px<0) px=0;
            if (px>128) px=128;
            if (py<0) py=0;
            if (py>64) py=64;
            
            #define ABS_MACRO(x) ((x)>0?(x):-(x))
            
            for(int i=0; i<20; i++) {
                if (dots_active[i] && ABS_MACRO(px-dots_x[i])<4 && ABS_MACRO(py-dots_y[i])<4) {
                    dots_active[i] = false; score++;
                }
            }
            if (px < gx) gx--; else if (px > gx) gx++;
            if (py < gy) gy--; else if (py > gy) gy++;
            
            if (ABS_MACRO(px-gx)<3 && ABS_MACRO(py-gy)<3) {
                px = 64; py = 32; gx = 20; gy = 20; score = 0;
                for(int i=0; i<20; i++) { dots_x[i] = esp_random()%120; dots_y[i] = esp_random()%60; dots_active[i] = true; }
            }
            for(int i=-3; i<=3; i++) for(int j=-3; j<=3; j++) { if(i*i+j*j<=9) draw_pixel(px+i, py+j, 1); }
            if (dir==0) draw_line(px, py, px-3, py-3, 0); 
            else if (dir==1) draw_line(px, py, px+3, py-3, 0);
            else if (dir==2) draw_line(px, py, px+3, py+3, 0);
            else if (dir==3) draw_line(px, py, px-3, py+3, 0);
            
            for(int i=-3; i<=3; i++) for(int j=-3; j<=3; j++) { if(j>-1 || i*i+j*j<=9) draw_pixel(gx+i, gy+j, 1); }
            for(int i=0; i<20; i++) if (dots_active[i]) draw_pixel(dots_x[i], dots_y[i], 1);
            
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score); draw_text(2,2,sb,1);
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_FROGGER) {
            static int fy = 60;
            static int fx = 64;
            static int cars_x[3] = {0, 60, 100};
            static int cars_y[3] = {40, 20, 10};
            static int speeds[3] = {2, -3, 4};
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            static bool p_left = false;
            static bool p_right = false;
            static int score = 0;
            if (last_mode != s_oled_mode) { fy = 60; fx = 64; score = 0; last_mode = s_oled_mode; }
            
            if (s_paddle_left && !p_left) fy -= 10;
            if (s_paddle_right && !p_right) fx += 10;
            p_left = s_paddle_left; p_right = s_paddle_right;
            if (fx > 120) fx = 120;
            if (fy < 0) { fy = 60; score++; speeds[0]+=1; speeds[1]-=1; speeds[2]+=1; }
            
            #define ABS_MACRO(x) ((x)>0?(x):-(x))
            
            for(int i=0; i<3; i++) {
                cars_x[i] += speeds[i];
                if (cars_x[i] > 140) cars_x[i] = -20;
                if (cars_x[i] < -20) cars_x[i] = 140;
                if (ABS_MACRO(fx-cars_x[i])<12 && ABS_MACRO(fy-cars_y[i])<8) {
                    fy = 60; score = 0; speeds[0]=2; speeds[1]=-3; speeds[2]=4;
                }
            }
            for(int i=-2; i<=2; i++) for(int j=-2; j<=2; j++) draw_pixel(fx+i, fy+j, 1);
            for(int i=0; i<3; i++) {
                for(int cx=-6; cx<=6; cx++) for(int cy=-4; cy<=4; cy++) draw_pixel(cars_x[i]+cx, cars_y[i]+cy, 1);
            }
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score); draw_text(2,2,sb,1);
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_RACING) {
            static int car_x = 64;
            static float track_pos = 0;
            static int score = 0;
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            if (last_mode != s_oled_mode) { car_x = 64; track_pos = 0; score = 0; last_mode = s_oled_mode; }
            
            if (s_paddle_left) car_x -= 3;
            if (s_paddle_right) car_x += 3;
            if (car_x < 0) car_x = 0;
            if (car_x > 128) car_x = 128;
            
            track_pos += 0.1f;
            float curve = sinf(track_pos * 0.5f) * 40.0f;
            
            int center = 64 + (int)curve;
            #define ABS_MACRO(x) ((x)>0?(x):-(x))
            if (ABS_MACRO(car_x - center) > 20) {
                score = 0;
            } else {
                score++;
            }
            
            for(int y=32; y<64; y+=4) {
                float sc = (y - 32) / 32.0f;
                int track_center = 64 + (int)(sinf(track_pos * 0.5f + (64-y)*0.05f) * 40.0f * sc);
                int w = 20 + (int)(20.0f * sc);
                draw_pixel(track_center - w, y, 1);
                draw_pixel(track_center + w, y, 1);
            }
            
            for(int i=-4; i<=4; i++) for(int j=0; j<8; j++) draw_pixel(car_x+i, 56+j, 1);
            
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score); draw_text(2,2,sb,1);
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_MATH) {
            static int n1 = 2, n2 = 2;
            static int ans = 4;
            static int score = 0;
            static bool p_left=false, p_right=false;
            static oled_mode_t last_mode = OLED_MODE_NORMAL;
            if (last_mode != s_oled_mode) { n1 = 2 + esp_random()%10; n2 = 2 + esp_random()%10; ans = n1*n2; score = 0; last_mode = s_oled_mode; }
            
            if (s_paddle_left && s_paddle_right) {
                if (!p_left && !p_right) {
                    if (ans == n1 * n2) {
                        score++; n1 = 2 + esp_random()%10; n2 = 2 + esp_random()%10; ans = n1*n2 + (esp_random()%5 - 2); 
                        if (ans < 0) ans = 0;
                    } else {
                        score = 0;
                    }
                }
            } else {
                if (s_paddle_left && !p_left) ans--;
                if (s_paddle_right && !p_right) ans++;
            }
            p_left = s_paddle_left; p_right = s_paddle_right;
            
            char sb[32]; snprintf(sb, sizeof(sb), "%d x %d = ?", n1, n2);
            draw_text(10, 20, sb, 1);
            snprintf(sb, sizeof(sb), "%d", ans);
            draw_text(60, 40, sb, 2);
            snprintf(sb, sizeof(sb), "Score: %d", score); draw_text(2,2,sb,1);
            
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(50));
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
