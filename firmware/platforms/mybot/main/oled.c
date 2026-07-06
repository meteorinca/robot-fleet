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
#include "buzzer.h"
#include "ultrasonic.h"
#include <math.h>
#include "config.h"

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

static volatile bool s_random_look_oled = false;
void oled_set_random_look(bool enable) { s_random_look_oled = enable; }

static volatile bool s_ap_client_just_connected = false;
void oled_notify_ap_client_connected(void) {
    s_ap_client_just_connected = true;
}


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

static void draw_pixel_portrait(int x, int y, int color) {
    draw_pixel(y, 63 - x, color);
}

static void draw_text_portrait(int start_x, int start_y, const char* text, int scale) {
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
                            draw_pixel_portrait(cx + gx * scale + dx, start_y + gy * scale + dy, 1);
                        }
                    }
                }
            }
        }
    }
}

static void draw_line_portrait(int x0, int y0, int x1, int y1, int color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int max_iters = 1000;
    while (max_iters--) {
        if (x0 >= 0 && x0 < 64 && y0 >= 0 && y0 < 128) {
            draw_pixel_portrait(x0, y0, color);
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

    // ── Face auto-change timer ─────────────────────────────────────────────
    // Normal: 5000 frames × 60ms ≈ 5 min. Random-look: 167 frames ≈ 10s.
    static int face_change_timer = 0;
    // Emotions to cycle (SAD removed from rotation — now just a dot anyway)
    static const eye_emotion_t face_cycle[] = {
        EYE_EMOTION_NORMAL, EYE_EMOTION_MAD, EYE_EMOTION_SLEEPY, EYE_EMOTION_NORMAL,
        EYE_EMOTION_SURPRISED, EYE_EMOTION_NORMAL
    };
    static int face_cycle_idx = 0;
    // Emotion-change blink transition (flutters eyes for 3 frames)
    static int emotion_blink_frames = 0;

    // ── Mouth micro-animation state ────────────────────────────────────────
    typedef enum { MOUTH_IDLE, MOUTH_YAWN, MOUTH_LICK } mouth_anim_state_t;
    static mouth_anim_state_t mouth_state = MOUTH_IDLE;
    static int   mouth_timer      = 0;   // frames in current state
    static float mouth_open       = 0.0f; // 0=closed 1=fully open
    static float mouth_lick_phase = 0.0f; // 0..1 sweep position
    static int   mouth_idle_tick  = 0;    // drives subtle idle twitch

    // ── Life-like quirk state ─────────────────────────────────────────────
    static int   quirk_squint_frames  = 0;  // >0 → eyes squinting
    static float quirk_squint_scale   = 1.0f;
    static bool  quirk_double_glance  = false; // mid-servo pause-reglance
    static int   quirk_fidget_timer   = 0; // eye micro-fidget without servo
    static int   quirk_fidget_dx      = 0, quirk_fidget_dy = 0;

    const int eye_cx[2] = { 32, 96 };
    const int eye_cy = 32;
    const int eye_rx = 26;
    const int base_ry = 26;
    const int base_pupil_r = 8;

    static oled_mode_t s_prev_tick_mode = OLED_MODE_NORMAL;
    while (1) {
        bool mode_just_changed = (s_prev_tick_mode != s_oled_mode);
        if (mode_just_changed) {
            bool was_us = (s_prev_tick_mode == OLED_MODE_US_SHOOTER || s_prev_tick_mode == OLED_MODE_ULTRASONIC_VIEW);
            bool is_us = (s_oled_mode == OLED_MODE_US_SHOOTER || s_oled_mode == OLED_MODE_ULTRASONIC_VIEW);
            if (was_us && !is_us) ultrasonic_set_active(false);
            if (!was_us && is_us) ultrasonic_set_active(true);
        }
        
        // Emulate last_mode for all the game blocks
        oled_mode_t last_mode = s_prev_tick_mode;
        s_prev_tick_mode = s_oled_mode;

        frame_count++;
        
        memset(s_buffer, 0, sizeof(s_buffer));

        if (s_ap_client_just_connected) {
            s_ap_client_just_connected = false;
            s_oled_mode = OLED_MODE_HOTSPOT_CONNECTED;
        }

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
        static wifi_state_t last_wstate = WIFI_STATE_CONNECTING;

        if (wstate != last_wstate) {
            if (wstate == WIFI_STATE_AP_MODE || wstate == WIFI_STATE_CONNECTED) {
                s_oled_mode = OLED_MODE_SHOW_IP;
            }
            last_wstate = wstate;
        }

        if (wstate == WIFI_STATE_CONNECTING && s_oled_mode == OLED_MODE_NORMAL) {
            draw_text(10, 24, "Connecting...", 1);
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_oled_mode == OLED_MODE_SHOW_IP) {
            static int ip_show_timer = 0;
            if (last_mode != s_oled_mode) {
                ip_show_timer = 0;
                last_mode = s_oled_mode;
            }
            ip_show_timer++;
            
            if (ip_show_timer >= 300) {
                s_oled_mode = OLED_MODE_NORMAL;
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            if (wstate == WIFI_STATE_AP_MODE) {
                draw_text(4, 0, "Hotspottin", 2);

                char name_buf[32];
                snprintf(name_buf, sizeof(name_buf), "Find me in WiFi:");
                draw_text(0, 18, name_buf, 1);

                snprintf(name_buf, sizeof(name_buf), "mybot-%d", DEVICE_NUMBER);
                draw_text(4, 30, name_buf, 2);

                /* show IP address */
                char ip_buf[20];
                snprintf(ip_buf, sizeof(ip_buf), "%d.%d.%d.%d",
                        192, 168, 4, 1);
                draw_text(-2, 50, ip_buf, 2);
            } else {
                char cd_buf[16];
                int sec_left = (300 - ip_show_timer) / 10;
                snprintf(cd_buf, sizeof(cd_buf), "%02d", sec_left);
                draw_text(115, 0, cd_buf, 1);
                
                char ip_buf[32];
                snprintf(ip_buf, sizeof(ip_buf), "%s", wifi_mgr_get_ip());
                draw_text(0, 20, ip_buf, 2);
                
                char mdns_buf[32];
                snprintf(mdns_buf, sizeof(mdns_buf), "%s.local", MDNS_HOSTNAME);
                draw_text(0, 44, mdns_buf, 1);
            }
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else if (s_oled_mode == OLED_MODE_HOTSPOT_CONNECTED) {
            static int hs_timer = 0;
            if (last_mode != s_oled_mode) {
                hs_timer = 0;
                last_mode = s_oled_mode;
            }
            hs_timer++;
            
            draw_text(10, 0, "Yayyyy!", 2);
            draw_text(4, 18, "You connected to me!", 1);

            int cx = 64, cy = 44;
            for (int a = 0; a < 360; a+=5) {
                float rad = a * 3.14159f / 180.0f;
                draw_pixel(cx + (int)(18 * cosf(rad)), cy + (int)(18 * sinf(rad)), 1);
            }
            draw_pixel(cx - 6, cy - 4, 1); draw_pixel(cx - 5, cy - 4, 1);
            draw_pixel(cx - 6, cy - 3, 1); draw_pixel(cx - 5, cy - 3, 1);
            draw_pixel(cx + 6, cy - 4, 1); draw_pixel(cx + 5, cy - 4, 1);
            draw_pixel(cx + 6, cy - 3, 1); draw_pixel(cx + 5, cy - 3, 1);
            for (int a = 20; a < 160; a+=5) {
                float rad = a * 3.14159f / 180.0f;
                draw_pixel(cx + (int)(10 * cosf(rad)), cy + 4 + (int)(10 * sinf(rad)), 1);
            }

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(100));

            if (hs_timer > 30) {
                s_oled_mode = OLED_MODE_FIREWORKS;
            }
            continue;
        }


        if (s_oled_mode == OLED_MODE_PONG_V || s_oled_mode == OLED_MODE_PONG_H) {
            static float ball_x = OLED_WIDTH / 2;
            static float ball_y = OLED_HEIGHT / 2;
            static float ball_dx = 2.0f;
            static float ball_dy = 2.0f;
            static float paddle_player_pos = OLED_WIDTH / 2;
            static float paddle_ai_pos = OLED_WIDTH / 2;
            
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
                        buzzer_play_tone(800, 20);
                    }
                }
                int ax = 2;
                if (ball_x <= ax + paddle_thick && ball_x + ball_size >= ax) {
                    if (ball_y + ball_size >= paddle_ai_pos - paddle_len/2 && ball_y <= paddle_ai_pos + paddle_len/2) {
                        ball_x = ax + paddle_thick;
                        ball_dx = -ball_dx;
                        buzzer_play_tone(600, 20);
                    }
                }
                
                // Score
                if (ball_x < 0) { score_p++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dx = 2.0f; buzzer_play_tone(1500, 100); }
                if (ball_x > OLED_WIDTH) { score_ai++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dx = -2.0f; buzzer_play_tone(200, 300); }
                
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
                        buzzer_play_tone(800, 20);
                    }
                }
                int ay = 2;
                if (ball_y <= ay + paddle_thick && ball_y + ball_size >= ay) {
                    if (ball_x + ball_size >= paddle_ai_pos - paddle_len/2 && ball_x <= paddle_ai_pos + paddle_len/2) {
                        ball_y = ay + paddle_thick;
                        ball_dy = -ball_dy;
                        buzzer_play_tone(600, 20);
                    }
                }
                
                // Score
                if (ball_y < 0) { score_p++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dy = 2.0f; buzzer_play_tone(1500, 100); }
                if (ball_y > OLED_HEIGHT) { score_ai++; ball_x = OLED_WIDTH/2; ball_y = OLED_HEIGHT/2; ball_dy = -2.0f; buzzer_play_tone(200, 300); }
                
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
                buzzer_play_tone(800, 30);
            }
            p_btn = btn;
            
            bird_dy += 0.3f;
            bird_y += bird_dy;
            if (bird_y < 0) { bird_y = 0; bird_dy = 0; }
            if (bird_y > 63) { 
                if (!game_over) buzzer_demo_gameover();
                game_over = true; 
            }
            
            pipe_x -= 3.0f;
            if (pipe_x < -14) {
                pipe_x = 128;
                pipe_gap_y = 15 + (esp_random() % 34);
                score++;
                buzzer_play_tone(1500, 50);
            }
            
            int bx = 20, by = (int)bird_y;
            int pw = 14, pg = 20;
            if (bx + 8 >= pipe_x && bx <= pipe_x + pw) {
                if (by <= pipe_gap_y - pg || by + 6 >= pipe_gap_y + pg) {
                    if (!game_over) buzzer_demo_gameover();
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
                buzzer_play_tone(600, 30);
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
                if (score % 10 == 0) buzzer_play_tone(1500, 100);
            }
            
            int dx = 20, dy = (int)dino_y;
            int cx = (int)cactus_x, cy = 48;
            if (dx + 6 >= cx && dx <= cx + 6) {
                if (dy + 8 >= cy) {
                    if (!game_over) buzzer_demo_gameover();
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
                if (!game_over) buzzer_demo_gameover();
                game_over = true;
            }
            
            for (int i=1; i<snake_len; i++) {
                if (snake_x[0] == snake_x[i] && snake_y[0] == snake_y[i]) {
                    if (!game_over) buzzer_demo_gameover();
                    game_over = true;
                }
            }
            
            if (snake_x[0] == food_x && snake_y[0] == food_y) {
                if (snake_len < 64) snake_len++;
                food_x = esp_random() % (128/3);
                food_y = esp_random() % (64/3);
                buzzer_play_tone(1800, 30);
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
                int px1 = 0, py1 = 0, px2 = 0, py2 = 0;
                bool first = true;
                for(int i=-40; i<=40; i+=2) {
                    float local_t = t + i * 0.15f;
                    vec3_t p1 = {(float)i, sinf(local_t)*10.0f, cosf(local_t)*10.0f};
                    vec3_t p2 = {(float)i, sinf(local_t + 3.14159f)*10.0f, cosf(local_t + 3.14159f)*10.0f};
                    p1 = rotate_y(p1, 0.3f); p2 = rotate_y(p2, 0.3f);
                    p1 = rotate_x(p1, 0.2f); p2 = rotate_x(p2, 0.2f);
                    int x1, y1, x2, y2;
                    bool proj1 = project_3d(p1, &x1, &y1);
                    bool proj2 = project_3d(p2, &x2, &y2);
                    if (proj1 && proj2) {
                        if (!first) {
                            draw_line(px1, py1, x1, y1, 1);
                            draw_line(px2, py2, x2, y2, 1);
                        }
                        if (i % 6 == 0) draw_line(x1, y1, x2, y2, 1);
                        px1 = x1; py1 = y1;
                        px2 = x2; py2 = y2;
                        first = false;
                    }
                }
            } else if (anim_idx == 2) { // Starfield
                draw_text(0, 0, "Starfield", 1);
                static vec3_t stars[60];
                static bool init = false;
                if (!init) {
                    for(int i=0; i<60; i++) {
                        stars[i] = (vec3_t){(float)((esp_random()%160)-80), (float)((esp_random()%160)-80), (float)(esp_random()%100)};
                    }
                    init = true;
                }
                for(int i=0; i<60; i++) {
                    float speed = 2.0f + (i % 3);
                    stars[i].z -= speed;
                    if (stars[i].z < 0) { 
                        stars[i] = (vec3_t){(float)((esp_random()%160)-80), (float)((esp_random()%160)-80), 100.0f}; 
                    }
                    vec3_t p = stars[i];
                    vec3_t tail = stars[i]; tail.z += speed * 2.5f;
                    p = rotate_z(p, t * 0.5f);
                    tail = rotate_z(tail, t * 0.5f);
                    int px, py, px_old, py_old;
                    if (project_3d(p, &px, &py) && project_3d(tail, &px_old, &py_old)) {
                        draw_line(px, py, px_old, py_old, 1);
                    }
                }
            } else if (anim_idx == 3) { // Torus
                draw_text(0, 0, "Torus", 1);
                float R = 15.0f;
                float r = 6.0f;
                const int num_theta = 12;
                const int num_phi = 8;
                int pts_x[12][8];
                int pts_y[12][8];
                bool pts_ok[12][8];
                for(int i=0; i<num_theta; i++) {
                    float theta = i * 6.283f / num_theta + t * 0.5f;
                    for(int j=0; j<num_phi; j++) {
                        float phi = j * 6.283f / num_phi + t * 2.0f;
                        vec3_t p = { (R + r * cosf(phi)) * cosf(theta), (R + r * cosf(phi)) * sinf(theta), r * sinf(phi) };
                        p = rotate_x(rotate_y(p, angle_y), angle_x);
                        pts_ok[i][j] = project_3d(p, &pts_x[i][j], &pts_y[i][j]);
                    }
                }
                for(int i=0; i<num_theta; i++) {
                    for(int j=0; j<num_phi; j++) {
                        if (!pts_ok[i][j]) continue;
                        int i_next = (i + 1) % num_theta;
                        int j_next = (j + 1) % num_phi;
                        if (pts_ok[i_next][j]) draw_line(pts_x[i][j], pts_y[i][j], pts_x[i_next][j], pts_y[i_next][j], 1);
                        if (pts_ok[i][j_next]) draw_line(pts_x[i][j], pts_y[i][j], pts_x[i][j_next], pts_y[i][j_next], 1);
                    }
                }
            } else if (anim_idx == 4) { // Wave Grid
                draw_text(0, 0, "Wave Grid", 1);
                const int grid_size = 9;
                int pts_x[9][9];
                int pts_y[9][9];
                bool pts_ok[9][9];
                int xi = 0;
                for(int x=-24; x<=24; x+=6, xi++) {
                    int zi = 0;
                    for(int z=-24; z<=24; z+=6, zi++) {
                        if (xi>=grid_size || zi>=grid_size) continue;
                        float y = sinf((x)*0.2f + t * 2.0f) * 4.0f + cosf((z)*0.2f + t * 1.5f) * 4.0f;
                        vec3_t p = rotate_x(rotate_y((vec3_t){(float)x, y, (float)z}, angle_y * 0.5f), 0.5f);
                        pts_ok[xi][zi] = project_3d(p, &pts_x[xi][zi], &pts_y[xi][zi]);
                    }
                }
                for(int i=0; i<grid_size; i++) {
                    for(int j=0; j<grid_size; j++) {
                        if (!pts_ok[i][j]) continue;
                        if (i+1 < grid_size && pts_ok[i+1][j]) {
                            draw_line(pts_x[i][j], pts_y[i][j], pts_x[i+1][j], pts_y[i+1][j], 1);
                        }
                        if (j+1 < grid_size && pts_ok[i][j+1]) {
                            draw_line(pts_x[i][j], pts_y[i][j], pts_x[i][j+1], pts_y[i][j+1], 1);
                        }
                    }
                }
            } else if (anim_idx == 5) { // Spirograph
                draw_text(0, 0, "Spirograph", 1);
                bool first = true;
                int prev_px = 0, prev_py = 0;
                for(float i=0; i<6.283f; i+=0.05f) {
                    float r = 12.0f * sinf(5.0f * i + t);
                    vec3_t p = { r * cosf(i), r * sinf(i), 6.0f * sinf(i * 3.0f + t * 2.0f) };
                    p = rotate_x(rotate_y(p, angle_y), angle_x);
                    int px, py;
                    if (project_3d(p, &px, &py)) {
                        if (!first) {
                            draw_line(prev_px, prev_py, px, py, 1);
                        }
                        prev_px = px; prev_py = py;
                        first = false;
                    }
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
            static const uint8_t frog_spr[8] = { 0x81, 0xC3, 0xFF, 0x5A, 0xFF, 0x3C, 0x66, 0xC3 };
            static const uint8_t car_spr[8]  = { 0x00, 0x7E, 0xFF, 0xDB, 0xFF, 0x7E, 0x3C, 0x00 };
            static int fy = 60;
            static int fx = 64;
            static int cars_x[3] = {0, 60, 100};
            static int cars_y[3] = {40, 20, 10};
            static int speeds[3] = {2, -3, 4};
            
            static bool p_left = false;
            static bool p_right = false;
            static int score = 0;
            if (last_mode != s_oled_mode) { fy = 60; fx = 64; score = 0; last_mode = s_oled_mode; }
            
            if (s_paddle_left && !p_left) { fy -= 10; buzzer_play_tone(1500, 20); }
            if (s_paddle_right && !p_right) { fx += 10; buzzer_play_tone(1500, 20); }
            p_left = s_paddle_left; p_right = s_paddle_right;
            if (fx > 120) fx = 120;
            if (fy < 0) { fy = 60; score++; speeds[0]+=1; speeds[1]-=1; speeds[2]+=1; buzzer_play_tone(2000, 50); }
            
            #define ABS_MACRO(x) ((x)>0?(x):-(x))
            
            for(int i=0; i<3; i++) {
                cars_x[i] += speeds[i];
                if (cars_x[i] > 140) cars_x[i] = -20;
                if (cars_x[i] < -20) cars_x[i] = 140;
                if (ABS_MACRO(fx-cars_x[i])<12 && ABS_MACRO(fy-cars_y[i])<8) {
                    fy = 60; score = 0; speeds[0]=2; speeds[1]=-3; speeds[2]=4;
                    buzzer_demo_gameover();
                }
            }
            
            draw_sprite8(fx - 4, fy - 4, frog_spr, 8, 8);
            for(int i=0; i<3; i++) {
                draw_sprite8(cars_x[i] - 6, cars_y[i] - 4, car_spr, 8, 8);
                draw_sprite8(cars_x[i] + 2, cars_y[i] - 4, car_spr, 8, 8);
            }
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score); draw_text(2,2,sb,1);
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_RACING) {
            static const uint8_t car_top[8] = { 0x18, 0x3C, 0x5A, 0x5A, 0x3C, 0x3C, 0x7E, 0x7E };
            static const uint8_t car_bot[8] = { 0x7E, 0x7E, 0x3C, 0x3C, 0x5A, 0x5A, 0x3C, 0x18 };
            static int car_x = 64;
            static float track_pos = 0;
            static int score = 0;
            
            if (last_mode != s_oled_mode) { car_x = 64; track_pos = 0; score = 0; last_mode = s_oled_mode; }
            
            if (s_paddle_left) { car_x -= 3; buzzer_play_tone(1200, 20); }
            if (s_paddle_right) { car_x += 3; buzzer_play_tone(1200, 20); }
            if (car_x < 0) car_x = 0;
            if (car_x > 128) car_x = 128;
            
            track_pos += 0.1f;
            float curve = sinf(track_pos * 0.5f) * 40.0f;
            
            int center = 64 + (int)curve;
            #define ABS_MACRO(x) ((x)>0?(x):-(x))
            if (ABS_MACRO(car_x - center) > 20) {
                if (score > 0) buzzer_demo_gameover();
                score = 0;
            } else {
                score++;
                if (score % 20 == 0) buzzer_play_tone(100, 30); // engine rumble effect
            }
            
            for(int y=20; y<64; y+=4) {
                float sc = (y - 20) / 44.0f;
                int track_center = 64 + (int)(sinf(track_pos * 0.5f + (64-y)*0.05f) * 40.0f * sc);
                int w = 15 + (int)(25.0f * sc);
                draw_line(track_center - w, y, track_center - w - 4, y+3, 1);
                draw_line(track_center + w, y, track_center + w + 4, y+3, 1);
            }
            
            draw_sprite8(car_x - 4, 48, car_top, 8, 8);
            draw_sprite8(car_x - 4, 56, car_bot, 8, 8);
            
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score); draw_text(2,2,sb,1);
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_MATH) {
            static int n1 = 2, n2 = 2;
            static int options[3];
            static int sel = 0;
            static int score = 0;
            static int timer = 100;
            static bool p_left=false, p_right=false;
            
            
            if (last_mode != s_oled_mode) { 
                n1 = 2 + esp_random() % 19; 
                n2 = 2 + esp_random() % 19; 
                sel = 0; score = 0; timer = 150;
                int correct_idx = esp_random() % 3;
                options[correct_idx] = n1 * n2;
                options[(correct_idx + 1) % 3] = n1 * n2 + (esp_random() % 10 + 1);
                options[(correct_idx + 2) % 3] = n1 * n2 - (esp_random() % 10 + 1);
                if (options[(correct_idx + 2) % 3] < 0) options[(correct_idx + 2) % 3] += 20;
                last_mode = s_oled_mode; 
            }
            
            if (timer <= 0) {
                // Game over
                draw_text_portrait(8, 50, "TIME UP!", 1);
                char sb[32]; snprintf(sb, sizeof(sb), "Score: %d", score);
                draw_text_portrait(10, 70, sb, 1);
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(100));
                if ((s_paddle_left && !p_left) || (s_paddle_right && !p_right)) {
                    score = 0; timer = 150;
                    n1 = 2 + esp_random() % 19; n2 = 2 + esp_random() % 19; 
                    int correct_idx = esp_random() % 3;
                    options[correct_idx] = n1 * n2;
                    options[(correct_idx + 1) % 3] = n1 * n2 + (esp_random() % 10 + 1);
                    options[(correct_idx + 2) % 3] = n1 * n2 - (esp_random() % 10 + 1);
                    if (options[(correct_idx + 2) % 3] < 0) options[(correct_idx + 2) % 3] += 20;
                }
                p_left = s_paddle_left; p_right = s_paddle_right;
                continue;
            }
            
            timer--;
            
            if (s_paddle_left && !p_left) sel = (sel + 1) % 3;
            if (s_paddle_right && !p_right) {
                if (options[sel] == n1 * n2) {
                    score++; timer += 30; if (timer > 150) timer = 150;
                    buzzer_play_tone(1500, 100);
                    n1 = 2 + esp_random() % 19; n2 = 2 + esp_random() % 19; 
                    sel = 0;
                    int correct_idx = esp_random() % 3;
                    options[correct_idx] = n1 * n2;
                    options[(correct_idx + 1) % 3] = n1 * n2 + (esp_random() % 10 + 1);
                    options[(correct_idx + 2) % 3] = n1 * n2 - (esp_random() % 10 + 1);
                    if (options[(correct_idx + 2) % 3] < 0) options[(correct_idx + 2) % 3] += 20;
                } else {
                    buzzer_demo_gameover();
                    timer = 0;
                }
            }
            p_left = s_paddle_left; p_right = s_paddle_right;
            
            char sb[32]; snprintf(sb, sizeof(sb), "%dx%d=?", n1, n2);
            draw_text_portrait(5, 15, sb, 2);
            
            for (int i=0; i<3; i++) {
                snprintf(sb, sizeof(sb), "%c %d", (i==sel)?'>':' ', options[i]);
                draw_text_portrait(10, 45 + i*20, sb, 2);
            }
            snprintf(sb, sizeof(sb), "Score:%d", score); draw_text_portrait(2,2,sb,1);
            
            // Draw timer bar
            draw_line_portrait(0, 126, (timer * 64) / 150, 126, 1);
            draw_line_portrait(0, 127, (timer * 64) / 150, 127, 1);
            
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        } else if (s_oled_mode == OLED_MODE_US_SHOOTER) {
            static float ship_y = 32.0f;
            static float asteroids_x[3] = {128, 160, 200};
            static float asteroids_y[3] = {10, 30, 50};
            static bool asteroids_active[3] = {true, true, true};
            static float lasers_x[3] = {0};
            static float lasers_y[3] = {0};
            static bool lasers_active[3] = {false};
            static int score = 0;
            static bool p_btn1 = false;
            static bool game_over = false;
            
            
            if (last_mode != s_oled_mode) {
                ship_y = 32.0f; score = 0; game_over = false;
                ultrasonic_set_active(true);
                for(int i=0; i<3; i++) {
                    asteroids_x[i] = 128 + i*40; asteroids_y[i] = esp_random()%54; asteroids_active[i] = true;
                    lasers_active[i] = false;
                }
                last_mode = s_oled_mode;
            }
            
            if (game_over) {
                draw_text(20, 20, "GAME OVER", 2);
                char sb[32]; snprintf(sb, sizeof(sb), "Score: %d", score);
                draw_text(30, 40, sb, 1);
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(100));
                if ((s_paddle_left && !p_btn1) || s_paddle_right) {
                    game_over = false; score = 0;
                    for(int i=0; i<3; i++) {
                        asteroids_x[i] = 128 + i*40; asteroids_y[i] = esp_random()%54; asteroids_active[i] = true;
                        lasers_active[i] = false;
                    }
                }
                p_btn1 = s_paddle_left;
                continue;
            }
            
            extern float ultrasonic_get_distance(void);
            float dist = ultrasonic_get_distance();
            if (dist > 2.0f && dist < 30.0f) {
                ship_y = ((dist - 2.0f) / 28.0f) * 60.0f;
            }
            if (ship_y < 0) ship_y = 0;
            if (ship_y > 60) ship_y = 60;
            
            if (s_paddle_left && !p_btn1) {
                for(int i=0; i<3; i++) {
                    if (!lasers_active[i]) {
                        lasers_active[i] = true;
                        lasers_x[i] = 12;
                        lasers_y[i] = ship_y + 2;
                        buzzer_play_tone(1500, 20);
                        break;
                    }
                }
            }
            p_btn1 = s_paddle_left;
            
            for(int i=0; i<3; i++) {
                if (lasers_active[i]) {
                    lasers_x[i] += 5.0f;
                    if (lasers_x[i] > 128) lasers_active[i] = false;
                    draw_line(lasers_x[i], lasers_y[i], lasers_x[i]-4, lasers_y[i], 1);
                }
            }
            
            #define ABS_MACRO(x) ((x)>0?(x):-(x))
            for(int i=0; i<3; i++) {
                if (asteroids_active[i]) {
                    asteroids_x[i] -= (2.0f + score*0.1f);
                    if (asteroids_x[i] < -10) {
                        asteroids_x[i] = 128 + (esp_random()%40);
                        asteroids_y[i] = esp_random()%54;
                    }
                    
                    for (int ax=-2; ax<=2; ax++) for (int ay=-2; ay<=2; ay++) {
                        if (ax*ax + ay*ay <= 4) draw_pixel(asteroids_x[i]+ax, asteroids_y[i]+ay, 1);
                    }
                    
                    for(int L=0; L<3; L++) {
                        if (lasers_active[L] && ABS_MACRO(lasers_x[L] - asteroids_x[i]) < 6 && ABS_MACRO(lasers_y[L] - asteroids_y[i]) < 6) {
                            asteroids_active[i] = false;
                            lasers_active[L] = false;
                            score++;
                            buzzer_play_tone(400, 30);
                            break;
                        }
                    }
                    
                    if (asteroids_active[i] && asteroids_x[i] < 12 && ABS_MACRO(ship_y + 2 - asteroids_y[i]) < 6) {
                        buzzer_demo_gameover();
                        game_over = true;
                    }
                } else {
                    asteroids_x[i] = 128 + (esp_random()%40);
                    asteroids_y[i] = esp_random()%54;
                    asteroids_active[i] = true;
                }
            }
            
            draw_line(2, ship_y, 10, ship_y+2, 1);
            draw_line(2, ship_y+4, 10, ship_y+2, 1);
            draw_line(2, ship_y, 2, ship_y+4, 1);
            
            char sb[32]; snprintf(sb, sizeof(sb), "%d", score); draw_text(110,2,sb,1);
            
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        } else if (s_oled_mode == OLED_MODE_MARIO_DANCE) {
            // ── Big Boo Ghost ────────────────────────────────────────────────
            static const uint32_t boo_frame0[32] = {
                0x00FFFF80, 0x03FFFFE0, 0x07FFFFF0, 0x0FFFFFF8,
                0x1FFFFFFC, 0x3FFFFFFE, 0x3FFFFFFE, 0x3FFFFFFE,
                0x7F81F81C, 0x7F00F00C, 0x7F00F00C, 0x7F00F00C,
                0x7F81F81C, 0x3FFFFFFE, 0x3FFFFFFE, 0x3CFFFF9E,
                0x3800000E, 0x3918C64E, 0x38E739CE, 0x3800000E,
                0x1E00003C, 0x1FFFFFFC, 0x0FFFFFF8, 0x0FFFFFF8,
                0x07FFFFF0, 0x07FFFFE0, 0x03FC7FC0, 0x01F83F80,
                0x00F01F00, 0x00000000, 0x00000000, 0x00000000,
            };
            static const uint32_t boo_frame1[32] = {
                0x00FFFF80, 0x03FFFFE0, 0x07FFFFF0, 0x0FFFFFF8,
                0x1FFFFFFC, 0x3FFFFFFE, 0x3FFFFFFE, 0x3FFFFFFE,
                0x7F81F81C, 0x7F00F00C, 0x7F20F20C, 0x7F00F00C,
                0x7F81F81C, 0x3FFFFFFE, 0x3FFFFFFE, 0x3CFFFF9E,
                0x3800000E, 0x3800000E, 0x3800000E, 0x3800000E,
                0x1E00003C, 0x1FFFFFFC, 0x0FFFFFF8, 0x0FFFFFF8,
                0x07FFFFF0, 0x07FFFFE0, 0x03FC7FC0, 0x01F83F80,
                0x00F01F00, 0x00000000, 0x00000000, 0x00000000,
            };

            static int anim_frame = 0;
            static int anim_tick = 0;
            

            if (last_mode != s_oled_mode) {
                anim_frame = 0; anim_tick = 0;
                buzzer_demo_mario();
                last_mode = s_oled_mode;
            }

            // Flip frame every 8 ticks (~240ms at 30ms frame)
            anim_tick++;
            if (anim_tick >= 8) {
                anim_frame ^= 1;
                anim_tick = 0;
            }

            const uint32_t *sprite = (anim_frame == 0) ? boo_frame0 : boo_frame1;

            int bob = (int)(sinf(frame_count * 0.25f) * 3.0f);

            int mx = 32, my = bob; 
            for (int r = 0; r < 32; r++) {
                uint32_t row = sprite[r];
                for (int c = 0; c < 32; c++) {
                    if (row & (1 << (31 - c))) {
                        draw_pixel(mx + c*2, my + r*2, 1);
                        draw_pixel(mx + c*2 + 1, my + r*2, 1);
                        draw_pixel(mx + c*2, my + r*2 + 1, 1);
                        draw_pixel(mx + c*2 + 1, my + r*2 + 1, 1);
                    }
                }
            }

            // Spinning stars around boo
            float t_star = frame_count * 0.12f;
            for (int i = 0; i < 6; i++) {
                float base_angle = i * 1.047f + t_star;
                int sx = 72 + (int)(cosf(base_angle) * 32.0f);
                int sy = 32 + (int)(sinf(base_angle) * 30.0f);
                draw_pixel(sx,   sy,   1);
                draw_pixel(sx+1, sy,   1);
                draw_pixel(sx,   sy+1, 1);
                draw_pixel(sx+1, sy+1, 1);
            }
            draw_text(2, 2, "BOO!", 1);
            draw_text(88, 54, "SPOOK!", 1);

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;

        } else if (s_oled_mode == OLED_MODE_FIREWORKS) {
            // ── Fireworks! ──────────────────────────────────────────────────
            #define FW_MAX 8
            typedef struct { float x, y, dx, dy; float life; bool active; } particle_t;
            static particle_t particles[FW_MAX * 12]; // 8 rockets * 12 sparks
            static float rocket_x[FW_MAX], rocket_y[FW_MAX], rocket_dy[FW_MAX];
            static bool rocket_active[FW_MAX];
            
            static int spawn_timer = 0;
            static int fw_timer = 0;

            if (last_mode != s_oled_mode) {
                for (int i = 0; i < FW_MAX; i++) rocket_active[i] = false;
                for (int i = 0; i < FW_MAX * 12; i++) particles[i].active = false;
                spawn_timer = 0;
                fw_timer = 0;
                last_mode = s_oled_mode;
            }

            fw_timer++;
            if (s_paddle_left || s_paddle_right || fw_timer > 166) {
                s_oled_mode = OLED_MODE_NORMAL;
            }

            // Spawn new rocket periodically
            spawn_timer++;
            if (spawn_timer >= 15) {
                spawn_timer = 0;
                for (int i = 0; i < FW_MAX; i++) {
                    if (!rocket_active[i]) {
                        rocket_x[i] = 10 + (esp_random() % 108);
                        rocket_y[i] = 63;
                        rocket_dy[i] = -(2.5f + (esp_random() % 10) * 0.3f);
                        rocket_active[i] = true;
                        break;
                    }
                }
            }

            // Update rockets
            for (int i = 0; i < FW_MAX; i++) {
                if (!rocket_active[i]) continue;
                rocket_y[i] += rocket_dy[i];
                draw_pixel((int)rocket_x[i], (int)rocket_y[i], 1);
                draw_pixel((int)rocket_x[i], (int)rocket_y[i]+1, 1);

                // Explode when reaching upper area
                if (rocket_y[i] < 8 + (esp_random() % 20)) {
                    rocket_active[i] = false;
                    buzzer_play_tone(600 + (esp_random() % 800), 50);
                    // Spawn sparks
                    int base = i * 12;
                    for (int p = 0; p < 12; p++) {
                        if (!particles[base + p].active) {
                            particles[base + p].active = true;
                            particles[base + p].x = rocket_x[i];
                            particles[base + p].y = rocket_y[i];
                            float angle = p * 0.5236f; // 30deg
                            float speed = 1.5f + (esp_random() % 10) * 0.2f;
                            particles[base + p].dx = cosf(angle) * speed;
                            particles[base + p].dy = sinf(angle) * speed * 0.6f;
                            particles[base + p].life = 1.0f;
                        }
                    }
                }
            }

            // Update & draw sparks
            for (int p = 0; p < FW_MAX * 12; p++) {
                if (!particles[p].active) continue;
                particles[p].x += particles[p].dx;
                particles[p].y += particles[p].dy;
                particles[p].dy += 0.08f; // gravity
                particles[p].life -= 0.05f;
                if (particles[p].life <= 0) { particles[p].active = false; continue; }
                if (particles[p].life > 0.5f) {
                    draw_pixel((int)particles[p].x, (int)particles[p].y, 1);
                    draw_pixel((int)particles[p].x+1, (int)particles[p].y, 1);
                } else {
                    draw_pixel((int)particles[p].x, (int)particles[p].y, 1);
                }
            }

            draw_text(28, 56, "FIREWORKS!", 1);

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;

        } else if (s_oled_mode == OLED_MODE_MATRIX_RAIN) {
            // ── Matrix Digital Rain ───────────────────────────────────────────
            #define MATRIX_COLS 21  // 128/6 columns ~21
            static int8_t col_heads[MATRIX_COLS];   // y position of head
            static int8_t col_lengths[MATRIX_COLS];  // trail length
            static int8_t col_speeds[MATRIX_COLS];   // frames per step
            static int8_t col_timers[MATRIX_COLS];
            static char   col_chars[MATRIX_COLS][32]; // chars in each trail
            

            if (last_mode != s_oled_mode) {
                for (int c = 0; c < MATRIX_COLS; c++) {
                    col_heads[c]   = -(esp_random() % 40);
                    col_lengths[c] = 5 + (esp_random() % 10);
                    col_speeds[c]  = 1 + (esp_random() % 3);
                    col_timers[c]  = 0;
                    for (int r = 0; r < 32; r++)
                        col_chars[c][r] = 33 + (esp_random() % 93); // printable ASCII
                }
                last_mode = s_oled_mode;
            }

            // Scroll each column
            for (int c = 0; c < MATRIX_COLS; c++) {
                col_timers[c]++;
                if (col_timers[c] >= col_speeds[c]) {
                    col_timers[c] = 0;
                    col_heads[c]++;
                    if (col_heads[c] > 64 + col_lengths[c]) {
                        col_heads[c]   = -(esp_random() % 30);
                        col_lengths[c] = 5 + (esp_random() % 10);
                        col_speeds[c]  = 1 + (esp_random() % 3);
                        for (int r = 0; r < 32; r++)
                            col_chars[c][r] = 33 + (esp_random() % 93);
                    }
                    // Randomly mutate chars in column
                    col_chars[c][esp_random() % 32] = 33 + (esp_random() % 93);
                }
            }

            // Draw each column trail (each char is 8px tall in our 1x scale)
            for (int c = 0; c < MATRIX_COLS; c++) {
                int x = c * 6;
                int head_y = col_heads[c];
                for (int t = 0; t < col_lengths[c]; t++) {
                    int y = head_y - t * 8;
                    if (y < 0 || y > 56) continue;
                    char ch_buf[2] = { col_chars[c][t % 32], 0 };
                    // Head pixel is bright (drawn normally), trail gets dimmer (skip pixels)
                    if (t == 0) {
                        draw_text(x, y, ch_buf, 1);
                    } else if (t < 3) {
                        draw_text(x, y, ch_buf, 1);
                    } else if (t % 2 == 0) {
                        // Dimmer: just draw a dot
                        draw_pixel(x + 2, y + 3, 1);
                    }
                }
            }

            draw_text(22, 56, "MATRIX RAIN", 1);

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;

        } else if (s_oled_mode == OLED_MODE_SPACE_INVADER) {
            // ── Space Invader Parade ──────────────────────────────────────────
            // Classic 11x8 invader sprites, 3 types, marching left-right
            static const uint8_t inv_a0[8] = { 0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x24, 0x5A, 0xA5 };
            static const uint8_t inv_a1[8] = { 0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x24, 0x42, 0x81 };
            static const uint8_t inv_b0[8] = { 0x0C, 0x7E, 0xDB, 0xFF, 0x5A, 0x24, 0x66, 0x00 };
            static const uint8_t inv_b1[8] = { 0x30, 0x7E, 0xDB, 0xFF, 0x5A, 0x24, 0x66, 0x00 };
            static const uint8_t inv_c0[8] = { 0x3C, 0x7E, 0xC3, 0xFF, 0x24, 0x66, 0xFF, 0x66 };
            static const uint8_t inv_c1[8] = { 0x3C, 0x7E, 0xC3, 0xFF, 0x24, 0x66, 0xFF, 0x24 };

            static int parade_x = 0;
            static int parade_dir = 1;
            static int parade_frame = 0;
            static int parade_tick = 0;
            static int laser_x = -1, laser_y = -1;
            static int shoot_timer = 0;
            

            if (last_mode != s_oled_mode) {
                parade_x = 0; parade_dir = 1; parade_frame = 0; parade_tick = 0;
                laser_x = -1; laser_y = -1; shoot_timer = 0;
                last_mode = s_oled_mode;
            }

            parade_tick++;
            if (parade_tick >= 6) {
                parade_tick = 0;
                parade_frame ^= 1;
                parade_x += parade_dir * 2;
                if (parade_x > 30 || parade_x < -2) {
                    parade_dir = -parade_dir;
                    // Drop down (bounce y instead of actual drop for screen room)
                }
            }

            // Draw 3 rows of invaders
            const uint8_t *sprites0[3][2] = {{inv_c0, inv_c1}, {inv_b0, inv_b1}, {inv_a0, inv_a1}};
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 4; col++) {
                    int ix = parade_x + col * 26;
                    int iy = 2 + row * 16;
                    if (ix < 0 || ix > 120) continue;
                    draw_sprite8(ix, iy, sprites0[row][parade_frame], 8, 8);
                }
            }

            // Laser shoot from bottom
            shoot_timer++;
            if (shoot_timer >= 25) {
                shoot_timer = 0;
                laser_x = 60 + (esp_random() % 20);
                laser_y = 63;
                buzzer_play_tone(300, 30);
            }
            if (laser_x >= 0) {
                laser_y -= 4;
                draw_pixel(laser_x, laser_y, 1);
                draw_pixel(laser_x, laser_y - 1, 1);
                draw_pixel(laser_x, laser_y - 2, 1);
                if (laser_y < 0) laser_x = -1;
            }

            // Player ship at bottom
            draw_sprite8(54, 55, inv_a0, 8, 8);

            draw_text(2, 56, "SPACE", 1);

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;

        } else if (s_oled_mode == OLED_MODE_HEARTBEAT) {
            // ── Heartbeat (~120 BPM lub-dub) ──────────────────────────────────
            static int      beat_frame  = 0;
            static float    heart_scale = 1.0f;
            static float    scale_vel   = 0.0f;
            static uint32_t ekg_pos     = 0;
            static bool     snd_lub = false, snd_dub = false;

            if (last_mode != s_oled_mode) {
                beat_frame = 0; heart_scale = 1.0f; scale_vel = 0.0f;
                ekg_pos = 0; snd_lub = false; snd_dub = false;
                last_mode = s_oled_mode;
            }

            beat_frame++;
            if (beat_frame >= 25) beat_frame = 0;

            float scale_target = 1.0f;
            if      (beat_frame < 3)  scale_target = 1.35f;
            else if (beat_frame < 6)  scale_target = 1.0f;
            else if (beat_frame < 9)  scale_target = 1.18f;

            // Spring-damper: snappy attack, silky release
            float spring = (scale_target - heart_scale) * 0.55f;
            scale_vel = scale_vel * 0.3f + spring;
            heart_scale += scale_vel;
            if (heart_scale < 0.85f) heart_scale = 0.85f;
            if (heart_scale > 1.42f) heart_scale = 1.42f;

            // Buzzer fires exactly once per beat event
            if (beat_frame == 0  && !snd_lub) { buzzer_play_tone(120, 30); snd_lub = true; }
            if (beat_frame == 1)                snd_lub = false;
            if (beat_frame == 7  && !snd_dub) { buzzer_play_tone(150, 40); snd_dub = true; }
            if (beat_frame == 8)                snd_dub = false;

            static const int8_t hw[] = {
                0, 3, 5, 7, 8, 9, 10, 11, 11, 12, 12, 11, 10,
                9, 8, 7, 6, 5, 4,  3,  2,  1,  0, -1, -1, -1, -1, -1
            };
            const int HN  = (int)(sizeof(hw) / sizeof(hw[0]));
            const int hcx = 64, hcy = 26;

            for (int r = 0; r < HN; r++) {
                int dy = r - 12;
                int w  = (int)(hw[r] * heart_scale);
                if (w <= 0) continue;
                int py = hcy + dy;
                if (py < 0 || py >= 50) continue;
                for (int dx = -w; dx <= w; dx++) {
                    int px = hcx + dx;
                    if (px >= 0 && px < OLED_WIDTH) draw_pixel(px, py, 1);
                }
            }

            // ── Pulsing ring (visible during lub and dub expansion) ───────────
            if ((beat_frame < 6) || (beat_frame >= 7 && beat_frame < 13)) {
                float rs = heart_scale * 1.38f;
                for (int r = 0; r < HN; r++) {
                    int dy = r - 12;
                    int w  = (int)(hw[r] * rs);
                    if (w <= 0) continue;
                    int py = hcy + dy;
                    if (py < 0 || py >= 50) continue;
                    int pxl = hcx - w, pxr = hcx + w;
                    if (pxl >= 0 && pxl < OLED_WIDTH) draw_pixel(pxl, py, 1);
                    if (pxr >= 0 && pxr < OLED_WIDTH) draw_pixel(pxr, py, 1);
                }
            }

            draw_text(10, 54, "my heart. it beats", 1);

            // ── EKG trace (scrolls left, unsigned counter = no negative modulo) ─────
            ekg_pos += 3;
            #define HB_EKG_PERIOD 64
            int prev_py = -1;
            for (int x = 0; x < OLED_WIDTH; x++) {
                int ph = (int)((ekg_pos + (uint32_t)x) % HB_EKG_PERIOD);
                int oy = 0;

                // P wave: smooth gentle bump
                if      (ph >= 8  && ph < 12) oy = -(ph - 8);
                else if (ph >= 12 && ph < 16) oy = -(16 - ph);

                // QRS complex: sharp spike -- Q dip, tall R, S recovery
                else if (ph == 22) oy =  2;
                else if (ph == 23) oy =  4;
                else if (ph == 24) oy =  0;
                else if (ph == 25) oy = -12;
                else if (ph == 26) oy = -24;
                else if (ph == 27) oy = -12;
                else if (ph == 28) oy =  4;
                else if (ph == 29) oy =  6;
                else if (ph == 30) oy =  2;
                else if (ph == 31) oy =  0;

                // T wave: rounded bump
                else if (ph >= 38 && ph < 42) oy = -(ph - 38);
                else if (ph >= 42 && ph < 46) oy = -(46 - ph);

                int py = 50 + oy;
                if (py < 0) py = 0;
                if (py >= OLED_HEIGHT) py = OLED_HEIGHT - 1;
                if (prev_py != -1) {
                    draw_line(x - 1, prev_py, x, py, 1);
                } else {
                    draw_pixel(x, py, 1);
                }
                prev_py = py;
            }
            #undef HB_EKG_PERIOD

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(20)); // 50fps, 25 frames = 500ms cycle (~120 BPM)
            continue;

        } else if (s_oled_mode == OLED_MODE_BUZZER_PIANO) {
            static bool p_left = false;
            static bool p_right = false;
            
            if (s_paddle_left && s_paddle_right) {
                if (!p_left || !p_right) buzzer_play_tone(784, 500); // G5
            } else if (s_paddle_left) {
                if (!p_left) buzzer_play_tone(523, 500); // C5
            } else if (s_paddle_right) {
                if (!p_right) buzzer_play_tone(659, 500); // E5
            } else {
                if (p_left || p_right) buzzer_play_tone(0, 50); // Stop tone early
            }
            
            p_left = s_paddle_left; p_right = s_paddle_right;
            
            draw_text(10, 10, "Buzzer Piano", 1);
            draw_text(10, 30, s_paddle_left ? "[X] Left" : "[ ] Left", 1);
            draw_text(10, 45, s_paddle_right ? "[X] Right" : "[ ] Right", 1);
            
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        } else if (s_oled_mode == OLED_MODE_ULTRASONIC_VIEW) {
            
            if (last_mode != s_oled_mode) {
                ultrasonic_set_active(true);
                last_mode = s_oled_mode;
            }
            
            extern float ultrasonic_get_distance(void);
            float dist = ultrasonic_get_distance();
            
            draw_text(30, 5, "ULTRASONIC", 1);
            draw_line(0, 16, 128, 16, 1);
            
            char sb[32]; 
            if (dist > 0.0f) {
                snprintf(sb, sizeof(sb), "%.1f cm", dist);
            } else {
                snprintf(sb, sizeof(sb), "-- cm");
            }
            draw_text(24, 30, sb, 2);
            
            // Draw radar arc based on distance
            if (dist > 0.0f && dist < 200.0f) {
                int r = (int)(dist / 10.0f);
                if (r < 1) r = 1;
                if (r > 20) r = 20;
                
                int t = (frame_count / 3) % 10;
                if (t < 5) {
                    draw_pixel(64, 55, 1); draw_pixel(63, 55, 1);
                    draw_pixel(65, 55, 1); draw_pixel(64, 54, 1);
                }
                
                for(int a=-10; a<=10; a++) {
                    float rad = a * 0.1f;
                    int hx = 64 + (int)(sinf(rad) * r * 2.0f);
                    int hy = 55 - (int)(cosf(rad) * r * 2.0f);
                    if (hx >= 0 && hx < 128 && hy >= 18 && hy < 64) {
                        draw_pixel(hx, hy, 1);
                    }
                }
            }
            
            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        } else if (s_oled_mode == OLED_MODE_MENU) {
            static const char* menu_opts[] = {
                "Pong (H)", "Pong (V)", "Flappy Bird", "Jumpy Dino", "Snake", 
                "Pacman", "Frogger", "Racing", "Math", "Spacesonic", "Piano", "3D Demo",
                "Mario", "Fireworks", "Matrix Rain", "Space Inv", "Heartbeat",
                "Ultrasonic", "Show IP", "Exit (Eyes)"
            };
            static const oled_mode_t menu_modes[] = {
                OLED_MODE_PONG_H, OLED_MODE_PONG_V, OLED_MODE_FLAPPY, OLED_MODE_DINO, OLED_MODE_SNAKE,
                OLED_MODE_PACMAN, OLED_MODE_FROGGER, OLED_MODE_RACING, OLED_MODE_MATH, OLED_MODE_US_SHOOTER,
                OLED_MODE_BUZZER_PIANO, OLED_MODE_3D_SHOWCASE,
                OLED_MODE_MARIO_DANCE, OLED_MODE_FIREWORKS, OLED_MODE_MATRIX_RAIN, OLED_MODE_SPACE_INVADER, OLED_MODE_HEARTBEAT,
                OLED_MODE_ULTRASONIC_VIEW, OLED_MODE_SHOW_IP, OLED_MODE_NORMAL
            };
            const int num_opts = 20;
            static int sel = 0;
            static bool p_left = false, p_right = false;
            
            
            if (last_mode != s_oled_mode) { sel = 0; last_mode = s_oled_mode; }
            
            if (s_paddle_left && !p_left) { sel = (sel + 1) % num_opts; buzzer_play_tone(1000, 20); }
            if (s_paddle_right && !p_right) {
                s_oled_mode = menu_modes[sel];
                buzzer_play_tone(1200, 20);
            }
            p_left = s_paddle_left; p_right = s_paddle_right;
            
            draw_text(38, 2, "MAIN MENU", 1);
            draw_line(0, 12, 128, 12, 1);
            
            for (int i = 0; i < 3; i++) {
                int idx = sel - 1 + i;
                if (idx >= 0 && idx < num_opts) {
                    if (i == 1) {
                        draw_text(2, 16 + i * 14, ">", 1);
                    }
                    draw_text(12, 16 + i * 14, menu_opts[idx], 1);
                }
            }
            
            oled_send_buffer(); vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        } else if (s_oled_mode == OLED_MODE_WIFI_RESET_CONFIRM) {
            // ── WiFi Reset Confirmation Screen ───────────────────────────────
            // Drawn while user decides. The actual wifi_forget_all() call is
            // made by main.c's button task on the confirming button press.
            // Auto-cancels after 10 s (167 frames × 60 ms) if no input.
            static int reset_confirm_timer = 0;
            if (last_mode != s_oled_mode) {
                reset_confirm_timer = 0;
            }
            reset_confirm_timer++;
            int secs_remaining = (167 - reset_confirm_timer) / 17 + 1;
            if (secs_remaining < 1) secs_remaining = 1;

            if (reset_confirm_timer > 167) {
                // Timed out — cancel silently and return to eyes
                reset_confirm_timer = 0;
                s_oled_mode = OLED_MODE_NORMAL;
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(60));
                continue;
            }

            // Header
            draw_text(14, 2,  "FORGET WiFi?", 1);
            draw_line(0, 12, 128, 12, 1);

            // Warning lines
            draw_text(4, 16, "Erases all saved", 1);
            draw_text(4, 26, "networks. Robot", 1);
            draw_text(4, 36, "will hotspot.", 1);

            // YES (highlighted — press boot btn again to confirm)
            draw_line(0, 46, 128, 46, 1);
            draw_text(8,  50, "[YES] Press btn", 1);
            draw_text(8,  58, "[ NO] Hold 7s...", 1);

            // Countdown in top-right
            char ct_buf[16];
            snprintf(ct_buf, sizeof(ct_buf), "%ds", secs_remaining);
            draw_text(104, 2, ct_buf, 1);

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        } else if (s_oled_mode == OLED_MODE_BIG_YAWN) {
            // ── 🥱 BIG YAWN — Exaggerated, animated, non-blocking ─────────
            // Phases: pre-yawn droop → dramatic open → peak hold → close → recovery
            // Each phase is computed per-frame; no blocking sleeps or nested loops.
            static int   by_frame    = 0;
            static float by_zzz_y[3];
            static int   by_zzz_x[3];
            static bool  by_zzz_on   = false;
            static int   by_shake    = 0;

            if (last_mode != s_oled_mode) {
                by_frame   = 0;
                by_shake   = 0;
                by_zzz_on  = false;
                for (int i = 0; i < 3; i++) {
                    by_zzz_y[i] = 50.0f - i * 8.0f;
                    by_zzz_x[i] = 84 + i * 10;
                }
                last_mode = s_oled_mode;
            }
            by_frame++;

            // ── Derive per-frame parameters from phase ──────────────────────────
            float mouth_t  = 0.0f; // 0=closed 1=max open
            float squint_t = 0.0f; // 0=normal 1=fully squinted
            float brow_lift = 0.0f; // eyebrow raise amount
            bool  show_innards = false;

            if (by_frame < 18) {
                // Phase 1: pre-yawn droop — eyes start to feel heavy
                float p     = by_frame / 18.0f;
                squint_t    = p * 0.30f;
                mouth_t     = p * 0.03f;
                brow_lift   = p * 2.0f;
            } else if (by_frame < 58) {
                // Phase 2: dramatic opening — smoothstep ease-in-out
                float p     = (by_frame - 18) / 40.0f;
                p           = p * p * (3.0f - 2.0f * p); // smoothstep
                squint_t    = 0.30f + p * 0.70f;
                mouth_t     = 0.03f + p * 0.97f;
                brow_lift   = 2.0f  + p * 6.0f; // brows shoot up dramatically
            } else if (by_frame < 85) {
                // Phase 3: PEAK HOLD — max open, teeth, tongue, ZZZs, shake
                squint_t     = 1.0f;
                mouth_t      = 1.0f;
                brow_lift    = 8.0f;
                show_innards = true;
                by_shake     = ((by_frame % 4) < 2) ? 1 : -1; // micro screen shake
                by_zzz_on    = true;
            } else if (by_frame < 128) {
                // Phase 4: closing — smoothstep ease-out
                float p      = (by_frame - 85) / 43.0f;
                p            = p * p * (3.0f - 2.0f * p);
                squint_t     = 1.0f - p * 0.85f;
                mouth_t      = 1.0f - p;
                brow_lift    = 8.0f * (1.0f - p);
                show_innards = (mouth_t > 0.45f);
                by_shake     = 0;
            } else if (by_frame < 148) {
                // Phase 5: recovery drowsy blinks
                squint_t  = ((by_frame - 128) % 10 < 4) ? 0.88f : 0.12f;
                mouth_t   = 0.0f;
                brow_lift = 0.0f;
                by_shake  = 0;
            } else {
                // Done — return to normal eyes
                by_frame   = 0;
                by_shake   = 0;
                by_zzz_on  = false;
                s_oled_mode = OLED_MODE_NORMAL;
                oled_send_buffer();
                vTaskDelay(pdMS_TO_TICKS(60));
                continue;
            }

            // ── Draw squinted eyes ────────────────────────────────────────────
            const int by_ecx[2] = { 32, 96 };
            const int by_ecy    = 27;  // slightly higher — head tilted back
            const int by_erx    = 24;
            int by_ery          = (int)(24.0f * (1.0f - squint_t * 0.88f));
            if (by_ery < 2) by_ery = 2;

            {
                long erx_sq = (long)by_erx * by_erx;
                long ery_sq = (long)by_ery * by_ery;
                long ey_lim = erx_sq * ery_sq;

                for (int ei = 0; ei < 2; ei++) {
                    int cx = by_ecx[ei];
                    // Pupil radius shrinks as eyes squint
                    int eprad = base_pupil_r - (int)(squint_t * 4.0f);
                    if (eprad < 3) eprad = 3;
                    int eprad_sq = eprad * eprad;

                    for (int y = by_ecy - by_ery; y <= by_ecy + by_ery; y++) {
                        if (y < 0 || y >= OLED_HEIGHT) continue;
                        int dy = y - by_ecy;
                        for (int x = cx - by_erx; x <= cx + by_erx; x++) {
                            if (x < 0 || x >= OLED_WIDTH) continue;
                            int dx = x - cx;
                            if ((long)dx*dx * ery_sq + (long)dy*dy * erx_sq > ey_lim) continue;

                            int pdx = dx - pupil_dx;
                            int pdy = dy - pupil_dy;
                            if (pdx*pdx + pdy*pdy <= eprad_sq) {
                                // pupil — black, skip
                            } else {
                                // Sclera — heavy eyelid during squint
                                bool draw_it = true;
                                // Top-eyelid droop: hide upper fraction based on squint
                                if (dy < -(int)(by_ery * (1.0f - squint_t * 0.70f)) + 1) draw_it = false;
                                if (draw_it) draw_pixel(x, y, 1);
                            }
                        }
                    }

                    // Catch-lights in pupils (always visible)
                    draw_pixel(cx + pupil_dx - 3, by_ecy + pupil_dy - 3, 1);
                    draw_pixel(cx + pupil_dx - 2, by_ecy + pupil_dy - 3, 1);
                    draw_pixel(cx + pupil_dx - 3, by_ecy + pupil_dy - 2, 1);

                    // ── Eyebrow: thick arc, rises dramatically then settles ──
                    int brow_base_y = by_ecy - by_ery - 4;
                    int brow_offset = -(int)brow_lift;
                    int brow_hw     = 14;
                    for (int xi = -brow_hw; xi <= brow_hw; xi++) {
                        // Arch: slightly curved
                        int by2 = brow_base_y + brow_offset - (xi * xi) / 55;
                        if (by2 >= 0 && by2 < OLED_HEIGHT) {
                            draw_pixel(cx + xi, by2,     1);
                            draw_pixel(cx + xi, by2 + 1, 1); // 2px thick
                        }
                    }
                }
            }

            // ── Draw HUGE mouth ─────────────────────────────────────────────
            {
                int mcx  = 64 + by_shake;
                int mcy  = 52;
                int m_hw = (int)(mouth_t * 36.0f);  // max half-width  36px
                int m_hh = (int)(mouth_t * 13.0f);  // max half-height 13px

                if (m_hw > 0 && m_hh > 0) {
                    long mhh_sq = (long)m_hh * m_hh;

                    // Draw ellipse outline row-by-row (no sqrtf loop - use Bresenham)
                    for (int dy = -m_hh; dy <= m_hh; dy++) {
                        int row_y = mcy + dy;
                        if (row_y < 0 || row_y >= OLED_HEIGHT) continue;
                        // x half-width at this dy via integer approximation
                        float frac = 1.0f - (float)(dy * dy) / (float)mhh_sq;
                        if (frac <= 0.0f) continue;
                        int dx = (int)(m_hw * sqrtf(frac));

                        // Left & right edges
                        draw_pixel(mcx - dx, row_y, 1);
                        draw_pixel(mcx + dx, row_y, 1);
                        // Solid lips: top 2 and bottom 2 rows of ellipse
                        if (dy <= -m_hh + 2 || dy >= m_hh - 2) {
                            for (int xi = -dx; xi <= dx; xi++)
                                draw_pixel(mcx + xi, row_y, 1);
                        }
                    }

                    // ── Internals (teeth, tongue, uvula) when wide enough ──
                    if (show_innards && m_hw > 12 && m_hh > 5) {
                        // TOP TEETH — white bar just inside top lip, with slots
                        int tt_y     = mcy - m_hh + 3;
                        int teeth_hw = (int)(m_hw * 0.72f);
                        for (int xi = -teeth_hw; xi <= teeth_hw; xi++) {
                            draw_pixel(mcx + xi, tt_y,     1);
                            draw_pixel(mcx + xi, tt_y + 1, 1);
                            // Tooth gap: erase slot every 6 px
                            if (abs(xi) % 6 == 5 && xi != -teeth_hw && xi != teeth_hw) {
                                draw_pixel(mcx + xi, tt_y,     0);
                                draw_pixel(mcx + xi, tt_y + 1, 0);
                            }
                        }

                        // BOTTOM TEETH — narrower row
                        int bt_y      = mcy + m_hh - 4;
                        int bteeth_hw = (int)(m_hw * 0.52f);
                        for (int xi = -bteeth_hw; xi <= bteeth_hw; xi++) {
                            draw_pixel(mcx + xi, bt_y,     1);
                            draw_pixel(mcx + xi, bt_y + 1, 1);
                        }

                        // TONGUE — small rounded tongue-tip above bottom lip
                        if (mouth_t > 0.68f) {
                            int tng_y  = mcy + m_hh - 8;
                            int tng_hw = (int)(m_hw * 0.28f);
                            for (int tdy = 0; tdy <= 4; tdy++) {
                                for (int tdx = -tng_hw; tdx <= tng_hw; tdx++) {
                                    // Round top corners
                                    if (tdy == 0 && abs(tdx) > tng_hw - 2) continue;
                                    draw_pixel(mcx + tdx, tng_y + tdy, 1);
                                }
                            }
                        }

                        // UVULA — dangles from centre of top teeth
                        if (mouth_t > 0.82f) {
                            int uv_x = mcx, uv_y = tt_y + 2;
                            draw_pixel(uv_x,     uv_y,     1);
                            draw_pixel(uv_x,     uv_y + 1, 1);
                            draw_pixel(uv_x,     uv_y + 2, 1);
                            draw_pixel(uv_x - 1, uv_y + 3, 1);
                            draw_pixel(uv_x,     uv_y + 3, 1);
                            draw_pixel(uv_x + 1, uv_y + 3, 1);
                            draw_pixel(uv_x - 1, uv_y + 4, 1);
                            draw_pixel(uv_x,     uv_y + 4, 1);
                            draw_pixel(uv_x + 1, uv_y + 4, 1);
                        }

                        // SOUND WAVES — animated ~ ripples from both corners
                        {
                            int wave_phase = by_frame % 6;
                            // Left waves (3 ripples, staggered)
                            for (int wi = 0; wi < 3; wi++) {
                                int wx = mcx - m_hw - 4 - wi * 5;
                                int wy = mcy + ((wave_phase + wi) % 3) - 1;
                                if (wx >= 0) {
                                    draw_pixel(wx,     wy,     1);
                                    draw_pixel(wx + 1, wy - 1, 1);
                                    draw_pixel(wx + 2, wy,     1);
                                    draw_pixel(wx + 3, wy + 1, 1);
                                    draw_pixel(wx + 4, wy,     1);
                                }
                            }
                            // Right waves (mirrored)
                            for (int wi = 0; wi < 3; wi++) {
                                int wx = mcx + m_hw + wi * 5;
                                int wy = mcy + ((wave_phase + wi) % 3) - 1;
                                if (wx + 4 < OLED_WIDTH) {
                                    draw_pixel(wx,     wy,     1);
                                    draw_pixel(wx + 1, wy - 1, 1);
                                    draw_pixel(wx + 2, wy,     1);
                                    draw_pixel(wx + 3, wy + 1, 1);
                                    draw_pixel(wx + 4, wy,     1);
                                }
                            }
                        }
                    }
                } else if (mouth_t > 0.0f) {
                    // Still tiny — just a thin parting line
                    int small_hw = (int)(mouth_t * 7.0f) + 1;
                    for (int xi = -small_hw; xi <= small_hw; xi++)
                        draw_pixel(mcx + xi, mcy + (xi * xi) / 14, 1);
                }
            }

            // ── ZZZs drifting upward and right ──────────────────────────────
            if (by_zzz_on) {
                static const char *z_labels[3] = { "z", "Z", "Z" };
                static const int   z_scale[3]  = { 1, 1, 2 };   // biggest Z is largest
                for (int i = 0; i < 3; i++) {
                    by_zzz_y[i] -= 0.45f;   // float upward
                    by_zzz_x[i] += (i & 1) ? 0 : 0; // slight rightward per-z drift could go here
                    int zy = (int)by_zzz_y[i];
                    int zx = by_zzz_x[i];
                    if (zy > -8 && zy < OLED_HEIGHT && zx > 0 && zx + 12 < OLED_WIDTH) {
                        draw_text(zx, zy, z_labels[i], z_scale[i]);
                    }
                }
                // Recycle when lead Z floats off top
                if (by_zzz_y[0] < -8.0f) {
                    for (int i = 0; i < 3; i++) {
                        by_zzz_y[i] = 50.0f - i * 8.0f;
                        by_zzz_x[i] = 84 + i * 10;
                    }
                }
            }

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }

        // ── Animated eyes ─────────────────────────────────────────────────

        // ── Face auto-change timer ────────────────────────────────────────
        {
            int face_threshold = s_random_look_oled ? 167 : 5000; // 10s or 5 min
            face_change_timer++;
            if (face_change_timer >= face_threshold) {
                face_change_timer = 0;
                face_cycle_idx = (face_cycle_idx + 1) % (int)(sizeof(face_cycle)/sizeof(face_cycle[0]));
                s_eye_emotion = face_cycle[face_cycle_idx];
                // Emotion-change flutter: 3-frame slow blink
                emotion_blink_frames = 3;
            }
        }

        // ── Life-like quirks (only when random look active) ───────────────
        if (s_random_look_oled) {
            // 1) Micro eye-fidget: occasionally shift pupils slightly w/o servo
            quirk_fidget_timer--;
            if (quirk_fidget_timer <= 0) {
                quirk_fidget_timer = 150 + (esp_random() % 250); // 9-24s between fidgets
                quirk_fidget_dx = ((int)(esp_random() % 5)) - 2;
                quirk_fidget_dy = ((int)(esp_random() % 3)) - 1;
            }

            // 2) Squint-surprise: rare — briefly squint then spring open
            if (quirk_squint_frames > 0) {
                quirk_squint_frames--;
                if (quirk_squint_frames > 5) {
                    quirk_squint_scale = 0.55f; // squint phase
                } else {
                    // Spring back (wide-open overshoot for a frame)
                    quirk_squint_scale = (quirk_squint_frames == 4) ? 1.12f : 1.0f;
                }
            } else {
                quirk_squint_scale = 1.0f;
                if (esp_random() % 400 == 0) quirk_squint_frames = 12;
            }
        } else {
            quirk_fidget_dx = 0; quirk_fidget_dy = 0;
            quirk_squint_scale = 1.0f;
        }

        // ── Blink logic ───────────────────────────────────────────────────
        blink_timer++;
        if (!blinking && blink_timer > next_blink) {
            blinking = true;
            blink_timer = 0;
        }

        // Emotion-blink flutter counts as a blink too
        if (emotion_blink_frames > 0) {
            blinking = true;
            emotion_blink_frames--;
        }

        // Slow blink (random look): 1-in-80 chance of slow blink (4 frames)
        static int slow_blink_hold = 0;
        if (s_random_look_oled && !blinking && slow_blink_hold <= 0 && esp_random() % 80 == 0) {
            slow_blink_hold = 4;
        }

        int max_ry = (int)(base_ry * quirk_squint_scale);
        if (blinking || slow_blink_hold > 0) {
            max_ry = 3;
            if (slow_blink_hold > 0) {
                slow_blink_hold--;
            } else if (blink_timer > 1 && emotion_blink_frames == 0) {
                blinking = false;
                blink_timer = 0;
                next_blink = 40 + (esp_random() % 100);
            }
        }

        // ── Pupil tracking ────────────────────────────────────────────────
        if (esp_random() % 20 == 0) {
            pupil_target_dx = (esp_random() % 14) - 7;
            pupil_target_dy = (esp_random() % 8) - 4;
        }
        if (pupil_dx < pupil_target_dx + quirk_fidget_dx) pupil_dx++;
        if (pupil_dx > pupil_target_dx + quirk_fidget_dx) pupil_dx--;
        if (pupil_dy < pupil_target_dy + quirk_fidget_dy) pupil_dy++;
        if (pupil_dy > pupil_target_dy + quirk_fidget_dy) pupil_dy--;

        int pupil_r = base_pupil_r + ((frame_count / 12) % 3) - 1;
        int pupil_r_sq = pupil_r * pupil_r;
        int rx_sq = eye_rx * eye_rx;
        int ry_sq = max_ry * max_ry;
        if (ry_sq < 1) ry_sq = 1;
        int ellipse_limit = rx_sq * ry_sq;

        // ── SAD eyelid modifier — heavy drooped top-lid ───────────────────
        // For sad emotion: top lid droops significantly (heavy, glassy look)
        // We trim the top of the sclera more aggressively than before.
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
                    bool in_pupil = (idx * idx + idy * idy <= pupil_r_sq);

                    if (in_pupil) {
                        // Pupil drawn as black (not set) — depth highlights added below
                    } else {
                        // Sclera
                        bool draw_it = true;
                        if (s_eye_emotion == EYE_EMOTION_MAD) {
                            // Angry: top-inner corner clipped — sharp V brow
                            if (dy < ((ei == 0) ? dx : -dx) / 2 - 4) draw_it = false;
                        } else if (s_eye_emotion == EYE_EMOTION_SAD) {
                            // Sad: top-outer corner drooped — wide puppy eyes
                            if (dy < ((ei == 0) ? -dx : dx) / 2 - 2) draw_it = false;
                        } else if (s_eye_emotion == EYE_EMOTION_SLEEPY) {
                            // Sleepy: top half hidden
                            if (dy < 0) draw_it = false;
                        }
                        if (draw_it) draw_pixel(x, y, 1);
                    }
                    break;
                }
            }
        }

        // ── Pupil depth highlights (glossy 3-D sheen) ─────────────────────
        // Two small white circles overlaid on each pupil: primary catch-light
        // (upper-left, r=2) and a tiny secondary dot (upper-right, r=1).
        for (int ei = 0; ei < 2; ei++) {
            int pcx = eye_cx[ei] + pupil_dx; // pupil centre x
            int pcy = eye_cy   + pupil_dy;   // pupil centre y

            // Primary catch-light — upper-left
            int hl1x = pcx - 3, hl1y = pcy - 3;
            for (int hdy = -2; hdy <= 2; hdy++) {
                for (int hdx = -2; hdx <= 2; hdx++) {
                    if (hdx*hdx + hdy*hdy <= 4) {
                        draw_pixel(hl1x + hdx, hl1y + hdy, 1);
                    }
                }
            }
            // Secondary dot — upper-right
            int hl2x = pcx + 3, hl2y = pcy - 2;
            draw_pixel(hl2x, hl2y, 1);
            draw_pixel(hl2x + 1, hl2y, 1);
        }

        // ── Mouth micro-animation state machine ───────────────────────────
        // States: MOUTH_IDLE (subtle breathing twitch),
        //         MOUTH_YAWN (rare wide open → close),
        //         MOUTH_LICK (rare sweep arc L→R).
        mouth_idle_tick++;
        switch (mouth_state) {
            case MOUTH_IDLE:
                // Very subtle open/close: sine-like using idle_tick, ~2 s cycle
                mouth_open = 0.0f; // stays tiny — just 1px twitch handled at draw
                // Rare triggers
                if (esp_random() % 400 == 0) {
                    mouth_state = MOUTH_YAWN;
                    mouth_timer = 0;
                    mouth_open  = 0.0f;
                } else if (esp_random() % 4000 == 0) {
                    mouth_state = MOUTH_LICK;
                    mouth_timer = 0;
                    mouth_lick_phase = 0.0f;
                }
                break;

            case MOUTH_YAWN:
                mouth_timer++;
                // Open: 0→1 over 30 frames, hold 10, close 30 frames
                if (mouth_timer < 30) {
                    mouth_open = mouth_timer / 30.0f;
                } else if (mouth_timer < 40) {
                    mouth_open = 1.0f;
                } else if (mouth_timer < 70) {
                    mouth_open = 1.0f - (mouth_timer - 40) / 30.0f;
                } else {
                    mouth_open  = 0.0f;
                    mouth_state = MOUTH_IDLE;
                    mouth_timer = 0;
                }
                break;

            case MOUTH_LICK:
                mouth_timer++;
                // Sweep phase 0→1 over 20 frames then done
                mouth_lick_phase = mouth_timer / 20.0f;
                if (mouth_timer > 20) {
                    mouth_state      = MOUTH_IDLE;
                    mouth_timer      = 0;
                    mouth_lick_phase = 0.0f;
                }
                break;
        }

        // ── Draw mouth ────────────────────────────────────────────────────
        {
            const int mx = 64;   // mouth center x
            const int my = 56;   // base y

            if (mouth_state == MOUTH_YAWN) {
                // Oval that opens vertically
                int open_px = (int)(mouth_open * 6.0f); // max 6px tall
                int half_w = 8;
                for (int xi = -half_w; xi <= half_w; xi++) {
                    // Top lip
                    draw_pixel(mx + xi, my - open_px / 2, 1);
                    // Bottom lip
                    draw_pixel(mx + xi, my + open_px / 2, 1);
                    // Sides
                    if (abs(xi) == half_w) {
                        for (int yi = -open_px/2; yi <= open_px/2; yi++)
                            draw_pixel(mx + xi, my + yi, 1);
                    }
                }
            } else if (mouth_state == MOUTH_LICK) {
                // Arc sweep from left to right
                int sweep_x = mx - 8 + (int)(mouth_lick_phase * 16.0f);
                // Draw the lick as a small teardrop moving across
                draw_pixel(sweep_x, my, 1);
                draw_pixel(sweep_x, my - 1, 1);
                draw_pixel(sweep_x + 1, my - 1, 1);
                // Plus the base tiny mouth behind
                int half_w = 5;
                for (int xi = -half_w; xi <= half_w; xi++) {
                    int py_m = my + (xi * xi) / 28;
                    draw_pixel(mx + xi, py_m, 1);
                }
            } else {
                // ── Static / idle mouth by emotion ───────────────────────
                // Subtle idle twitch: every ~120 frames, add 1 extra row
                bool idle_twitch = ((mouth_idle_tick / 120) % 2 == 0) && (mouth_idle_tick % 120 > 110);

                if (s_eye_emotion == EYE_EMOTION_NORMAL || s_eye_emotion == EYE_EMOTION_SURPRISED) {
                    // Tiny subtle smile: very shallow arc, half-width 6 px
                    int half_w = 6;
                    for (int xi = -half_w; xi <= half_w; xi++) {
                        int py_m = my + (xi * xi) / 28; // very shallow curve
                        draw_pixel(mx + xi, py_m, 1);
                        if (idle_twitch) draw_pixel(mx + xi, py_m + 1, 1);
                    }
                } else if (s_eye_emotion == EYE_EMOTION_MAD) {
                    // Flat grimace: 7 px wide nearly straight
                    int half_w = 7;
                    for (int xi = -half_w; xi <= half_w; xi++) {
                        int py_m = my + (xi * xi) / 60; // almost flat
                        draw_pixel(mx + xi, py_m, 1);
                    }
                    // Corner ticks for severity
                    draw_pixel(mx - half_w - 1, my + 1, 1);
                    draw_pixel(mx + half_w + 1, my + 1, 1);
                } else if (s_eye_emotion == EYE_EMOTION_SAD) {
                    // Just a tiny 3-pixel dot — no frown, minimal sadness
                    draw_pixel(mx - 1, my, 1);
                    draw_pixel(mx,     my, 1);
                    draw_pixel(mx + 1, my, 1);
                    // Idle twitch: dot briefly extends to a tiny flat line
                    if (idle_twitch) {
                        draw_pixel(mx - 2, my, 1);
                        draw_pixel(mx + 2, my, 1);
                    }
                } else if (s_eye_emotion == EYE_EMOTION_SLEEPY) {
                    // Tiny 5-pixel flat line — too tired to smile
                    draw_pixel(mx - 2, my, 1);
                    draw_pixel(mx - 1, my, 1);
                    draw_pixel(mx,     my, 1);
                    draw_pixel(mx + 1, my, 1);
                    draw_pixel(mx + 2, my, 1);
                } else {
                    // Fallback: single dot
                    draw_pixel(mx, my, 1);
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
        0x8D, 0x14, 0x20, 0x00, 0xA0, 0xC0, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    for (int i = 0; i < sizeof(init_cmds); i++) {
        oled_send_cmd(init_cmds[i]);
    }

    xTaskCreate(oled_eyes_task, "oled_eyes", 4096, NULL, 1, NULL);
}

#endif
