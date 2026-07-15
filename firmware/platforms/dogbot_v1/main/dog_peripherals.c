#include "dog_peripherals.h"
#include "config.h"
// audio_clips.h removed — clip_chirp/clip_bark unused in dogbot_v1 (saves ~34KB flash)
#include "paulbot_audio_8bit.h"
#include "dogbark_audio_8bit.h"
#include "extra_sounds.h"
#include "font5x7.h"
#include <math.h>

#ifdef DISP_MOSI_GPIO

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_random.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>

static const char *TAG = "DOG_PERIPH";

static volatile bool force_blink = false;
static volatile int eye_mood = 0; // 0=happy (full), 1=angry, 2=neutral, 3=sad
static QueueHandle_t button_evt_queue = NULL;

static char oled_text_msg[64] = {0};
static volatile int oled_text_timer = 0;

// Display animation mode: 0=eyes(default), 1=fireworks, 2=matrix, 3=heartbeat
static volatile int dog_display_mode = 0;
static volatile int dog_anim_timer   = 0; // >0: auto-expire after N frames; -1: permanent

static void dog_draw_line(uint16_t* buf, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1), sx = x0 < x1 ? 1 : -1;
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1)), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        if (x0 >= 0 && x0 < 160 && y0 >= 0 && y0 < 80) buf[y0 * 160 + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void dog_set_display_mode(int mode) {
    dog_display_mode = (mode >= 0 && mode <= 3) ? mode : 0;
    dog_anim_timer   = -1; // permanent until changed
}

void dog_set_display_mode_timed(int mode, int duration_ms) {
    dog_display_mode = (mode >= 0 && mode <= 3) ? mode : 0;
    dog_anim_timer   = duration_ms / 60; // ~60ms per frame
}

void dog_set_eye_mood(int mood) {
    if (mood >= 0 && mood <= 3) eye_mood = mood;
}

void dog_set_oled_text(const char* msg, int duration_ms) {
    strncpy(oled_text_msg, msg, sizeof(oled_text_msg) - 1);
    oled_text_msg[sizeof(oled_text_msg) - 1] = '\0';
    if (duration_ms < 0) {
        oled_text_timer = -1;
    } else {
        oled_text_timer = duration_ms / 60; // ~60ms per frame
    }
}

void dog_dismiss_oled(void) {
    oled_text_timer = 0;
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



static void draw_text_autoscale(uint16_t *buffer, const char* text) {
    static char last_text[64] = {0};
    static uint32_t frame_tick = 0;
    
    if (strncmp(text, last_text, 64) != 0) {
        strncpy(last_text, text, 64);
        frame_tick = 0;
    }
    frame_tick++;

    char temp[64];
    strncpy(temp, text, sizeof(temp)-1);
    temp[sizeof(temp)-1] = '\0';
    
    int num_lines = 1;
    for(int i=0; temp[i]; i++) {
        if(temp[i] == '\n') num_lines++;
    }
    
    char *lines[4];
    int line_idx = 0;
    char *p = temp;
    lines[line_idx++] = p;
    for(int i=0; p[i] && line_idx < 4; i++) {
        if(p[i] == '\n') {
            p[i] = '\0';
            lines[line_idx++] = &p[i+1];
        }
    }
    
    int line_scale[4] = {1, 1, 1, 1};
    int total_h = 0;
    
    for(int l=0; l<num_lines; l++) {
        int len = strlen(lines[l]);
        if (len > 0) {
            line_scale[l] = 160 / (6 * len - 1);
            if (line_scale[l] > 8) line_scale[l] = 8;
            if (line_scale[l] < 2) line_scale[l] = 2; // minimum scale 2 for readability!
        }
        total_h += 8 * line_scale[l];
    }
    
    while(total_h > 80) {
        int max_scale = 0;
        int max_l = -1;
        for (int l = 0; l < num_lines; l++) {
            if (line_scale[l] > max_scale && line_scale[l] > 1) {
                max_scale = line_scale[l];
                max_l = l;
            }
        }
        if (max_l == -1) break;
        line_scale[max_l]--;
        total_h -= 8;
    }
    
    int current_y = (80 - total_h) / 2;
    
    for(int l=0; l<num_lines; l++) {
        int len = strlen(lines[l]);
        int scale = line_scale[l];
        int total_w = len * 6 * scale - 1 * scale;
        int start_x = (160 - total_w) / 2;
        
        if (total_w > 160) {
            int extra_w = total_w - 160 + 8; // 8 pixels padding
            int pause = 30; // 30 frames pause (~1.8 seconds)
            int cycle = frame_tick % ((extra_w + pause) * 2);
            int offset = 0;
            if (cycle < pause) offset = 0;
            else if (cycle < pause + extra_w) offset = cycle - pause;
            else if (cycle < pause * 2 + extra_w) offset = extra_w;
            else offset = extra_w - (cycle - (pause * 2 + extra_w));
            
            start_x = 4 - offset;
        }
        
        for (int i = 0; i < len; i++) {
            char c = lines[l][i];
            if (c < 32 || c > 126) c = 32;
            const uint8_t *glyph = &font5x7[(c - 32) * 8];
            int cx = start_x + i * 6 * scale;
            for (int gx = 0; gx < 5; gx++) {
                uint8_t col = glyph[gx];
                for (int gy = 0; gy < 7; gy++) {
                    if (col & (1 << gy)) {
                        for (int dx = 0; dx < scale; dx++) {
                            for (int dy = 0; dy < scale; dy++) {
                                int px = cx + gx * scale + dx;
                                int py = current_y + gy * scale + dy;
                                if (px >= 0 && px < 160 && py >= 0 && py < 80) {
                                    buffer[py * 160 + px] = 0xFFFF;
                                }
                            }
                        }
                    }
                }
            }
        }
        current_y += 8 * scale;
    }
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

        /* ---- Render both eyes / animations ---- */
        if (oled_text_timer > 0 || oled_text_timer == -1) {
            // OLED text overlay — highest priority
            if (oled_text_timer > 0) oled_text_timer--;
            memset(buffer, 0, 160 * 80 * sizeof(uint16_t));
            draw_text_autoscale(buffer, oled_text_msg);
            vTaskDelay(1);
        } else if (dog_display_mode == 1) {
            // ════ FIREWORKS (FAST & MULTIPLE) ════
            #define MAX_FW 4
            static int fw_phase[MAX_FW] = {0};
            static float fw_x[MAX_FW], fw_y[MAX_FW], fw_dy[MAX_FW];
            static float spark_x[MAX_FW][24], spark_y[MAX_FW][24], spark_dx[MAX_FW][24], spark_dy[MAX_FW][24];
            static uint16_t fw_color[MAX_FW] = {0};
            
            // Fade background
            for (int i = 0; i < 160 * 80; i++) {
                uint16_t p = buffer[i];
                uint8_t r = ((p >> 11) & 0x1F), g = ((p >> 5)  & 0x3F), b = (p & 0x1F);
                if (r > 0) r--;
                if (g > 1) g -= 2;
                if (b > 0) b--;
                buffer[i] = (uint16_t)((r << 11) | (g << 5) | b);
            }

            if (frame_count % 12 == 0) { // launch often
                for (int f = 0; f < MAX_FW; f++) {
                    if (fw_phase[f] == 0) {
                        fw_x[f] = 20 + (fast_rand() % 120); fw_y[f] = 80;
                        fw_dy[f] = -(3.0f + (fast_rand() % 15) * 0.1f);
                        fw_color[f] = rgb565(150 + fast_rand() % 105, 150 + fast_rand() % 105, 150 + fast_rand() % 105);
                        fw_phase[f] = 1;
                        break;
                    }
                }
            }

            for (int f = 0; f < MAX_FW; f++) {
                if (fw_phase[f] == 1) {
                    fw_y[f] += fw_dy[f]; fw_dy[f] += 0.12f;
                    int px = (int)fw_x[f], py = (int)fw_y[f];
                    if (px >= 0 && px < 160 && py >= 0 && py < 80) {
                        buffer[py * 160 + px] = fw_color[f];
                        buffer[py * 160 + px + 1] = fw_color[f];
                        if (py+1 < 80) buffer[(py+1) * 160 + px] = fw_color[f];
                    }
                    if (fw_dy[f] >= -0.2f) { // Apex
                        fw_phase[f] = 2;
                        for (int i = 0; i < 24; i++) {
                            spark_x[f][i] = fw_x[f]; spark_y[f][i] = fw_y[f];
                            float angle = i * 6.283f / 24.0f;
                            float speed = 1.5f + (fast_rand() % 20) * 0.1f;
                            spark_dx[f][i] = cosf(angle) * speed;
                            spark_dy[f][i] = sinf(angle) * speed;
                        }
                    }
                } else if (fw_phase[f] == 2) {
                    bool active = false;
                    for (int i = 0; i < 24; i++) {
                        spark_x[f][i] += spark_dx[f][i]; spark_y[f][i] += spark_dy[f][i];
                        spark_dy[f][i] += 0.05f;
                        spark_dx[f][i] *= 0.92f; spark_dy[f][i] *= 0.92f;
                        int sx = (int)spark_x[f][i], sy = (int)spark_y[f][i];
                        if (sy < 80 && spark_dx[f][i]*spark_dx[f][i] + spark_dy[f][i]*spark_dy[f][i] > 0.02f) active = true;
                        if (sx >= 0 && sx < 160 && sy >= 0 && sy < 80) {
                            buffer[sy * 160 + sx] = fw_color[f];
                            buffer[sy * 160 + sx + 1] = fw_color[f]; // 2x1 thick
                        }
                    }
                    if (!active) fw_phase[f] = 0;
                }
            }
            if (dog_anim_timer > 0) { dog_anim_timer--; if (dog_anim_timer == 0) { dog_display_mode = 0; for(int f=0;f<MAX_FW;f++) fw_phase[f]=0; } }
            vTaskDelay(1);

        } else if (dog_display_mode == 2) {
            // ════ MATRIX RAIN ════
            #define MX_COLS 26  // 160/6 columns
            static uint8_t  mx_head[MX_COLS], mx_speed[MX_COLS], mx_timer[MX_COLS], mx_chars[MX_COLS];
            static bool     mx_init = false;
            static int      mx_hue = 120; // start green
            
            if (!mx_init) {
                for (int c = 0; c < MX_COLS; c++) {
                    mx_head[c]  = (uint8_t)(fast_rand() % 80);
                    mx_speed[c] = 2 + (uint8_t)(fast_rand() % 3); // slower
                    mx_timer[c] = mx_speed[c];
                    mx_chars[c] = (uint8_t)(fast_rand() % 95);
                }
                mx_init = true;
            }
            if (frame_count % 5 == 0) mx_hue = (mx_hue + 1) % 360; // slow color shift

            // HSV -> RGB logic
            int sector = mx_hue / 60, frac = (mx_hue % 60) * 255 / 60;
            uint8_t hr, hg, hb;
            switch (sector) {
                case 0: hr=255; hg=frac;    hb=0;       break;
                case 1: hr=255-frac; hg=255; hb=0;      break;
                case 2: hr=0;   hg=255;    hb=frac;     break;
                case 3: hr=0;   hg=255-frac; hb=255;    break;
                case 4: hr=frac; hg=0;    hb=255;       break;
                default:hr=255; hg=0;    hb=255-frac;   break;
            }
            uint16_t head_color = rgb565(hr, hg, hb);
            uint16_t dim_color = rgb565(hr/4, hg/4, hb/4);

            // Fade background
            for (int i = 0; i < 160 * 80; i++) {
                uint16_t p = buffer[i];
                uint8_t r = ((p >> 11) & 0x1F), g = ((p >> 5)  & 0x3F), b = (p & 0x1F);
                if (r > 0) r--;
                if (g > 1) g -= 2;
                if (b > 0) b--;
                buffer[i] = (uint16_t)((r << 11) | (g << 5) | b);
            }

            for (int c = 0; c < MX_COLS; c++) {
                int cx = c * 6, row = mx_head[c];
                mx_chars[c] = (uint8_t)((mx_chars[c] + 1) % 95);
                char ch = 32 + mx_chars[c];
                if (ch < 32 || ch > 126) ch = 65;
                const uint8_t *glyph = &font5x7[(ch - 32) * 8];
                for (int gx = 0; gx < 5; gx++) {
                    uint8_t col = glyph[gx];
                    for (int gy = 0; gy < 7; gy++) {
                        int px = cx + gx, py = row + gy;
                        if (px < 160 && py < 80)
                            buffer[py * 160 + px] = col & (1 << gy) ? head_color : dim_color;
                    }
                }
                if (--mx_timer[c] == 0) {
                    mx_timer[c] = mx_speed[c];
                    mx_head[c] = (uint8_t)((row + 8) % 80);
                }
            }
            if (dog_anim_timer > 0) { dog_anim_timer--; if (dog_anim_timer == 0) { dog_display_mode = 0; mx_init = false; } }
            vTaskDelay(1);

        } else if (dog_display_mode == 3) {
            // ════ HEARTBEAT PULSE ════
            typedef struct { float x, y; } vec2_t;
            static int beat_frame = 0;
            static float heart_scale = 1.0f, scale_vel = 0.0f;
            
            // Fade background (reddish fade)
            for (int i = 0; i < 160 * 80; i++) {
                uint16_t p = buffer[i];
                uint8_t r = ((p >> 11) & 0x1F), g = ((p >> 5)  & 0x3F), b = (p & 0x1F);
                if (r > 1) r -= 2;
                if (g > 0) g--;
                if (b > 0) b--;
                buffer[i] = (uint16_t)((r << 11) | (g << 5) | b);
            }

            beat_frame++;
            if (beat_frame >= 25) beat_frame = 0;
            float scale_target = 1.0f;
            if (beat_frame < 3) scale_target = 1.35f;
            else if (beat_frame < 6) scale_target = 1.0f;
            else if (beat_frame < 9) scale_target = 1.18f;

            scale_vel = scale_vel * 0.3f + (scale_target - heart_scale) * 0.55f;
            heart_scale += scale_vel;
            if (heart_scale < 0.85f) heart_scale = 0.85f;
            if (heart_scale > 1.42f) heart_scale = 1.42f;

            vec2_t b1[4] = {{-64, 5}, {-32, -10}, {-16, 16}, {0, 10}};
            vec2_t b2[4] = {{0, 10},  {16, 4},    {30, -20}, {0, -5}};
            vec2_t b3[4] = {{0, -5},  {-30, -20}, {-16, 4},  {0, 10}};
            vec2_t b4[4] = {{0, 10},  {16, 16},   {32, -10}, {64, 5}};

            int prev_x = -1, prev_y = -1;
            uint16_t hb_color = rgb565(255, 30, 50);
            for (int seg = 0; seg < 4; seg++) {
                vec2_t *b = (seg == 0) ? b1 : (seg == 1) ? b2 : (seg == 2) ? b3 : b4;
                for (int i = 0; i <= 32; i++) {
                    float t = (float)i / 32.0f, u = 1.0f - t;
                    float tt = t * t, uu = u * u;
                    float bx = u*uu * b[0].x + 3 * uu * t * b[1].x + 3 * u * tt * b[2].x + t*tt * b[3].x;
                    float by = u*uu * b[0].y + 3 * uu * t * b[1].y + 3 * u * tt * b[2].y + t*tt * b[3].y;
                    
                    float blend = 1.0f;
                    if (seg == 0) blend = t;
                    else if (seg == 3) blend = 1.0f - t;
                    
                    float float_y = sinf(frame_count * 0.05f + bx * 0.02f) * 2.0f;
                    float scale = 1.0f + (heart_scale - 1.0f) * blend;
                    int px = 80 + (int)(bx * scale); // centered at 80
                    int py = 40 + (int)(by * scale + float_y); // centered at 40

                    if (prev_x != -1) dog_draw_line(buffer, prev_x, prev_y, px, py, hb_color);
                    prev_x = px; prev_y = py;
                }
            }
            if (dog_anim_timer > 0) { dog_anim_timer--; if (dog_anim_timer == 0) { dog_display_mode = 0; } }
            vTaskDelay(1);

        } else {
            // ════ MODE 0: EYES (default) ════
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
                            r = 100; g = 200; b = 255;
                        } else if (frac > 120) {
                            r = 80; g = 180; b = 240;
                        } else if (frac > 50) {
                            r = 60; g = 150; b = 220;
                        } else {
                            r = 40; g = 120; b = 200;
                        }

                        uint32_t rr = fast_rand();
                        if ((rr & 0xF) == 0) {
                            r = 255; g = 200; b = 100; // gold sparkle
                        }
                        if ((rr & 0x1F) == 1) {
                            r = 100; g = 255; b = 200; // cyan sparkle
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

            /* ---- Tiny Minimal Mouth ---- */
            int mouth_w = 2 + ((frame_count / 12) % 2);
            int mouth_h = 1 + ((frame_count / 25) % 2);
            int mouth_y = 58; 
            for (int my = mouth_y; my < mouth_y + mouth_h; my++) {
                for (int mx = 80 - mouth_w; mx <= 80 + mouth_w; mx++) {
                    buffer[my * 160 + mx] = warm_edge;
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
    bool stream_active = false;
    bool amp_enabled = false;

    while(1) {
        size_t item_size = 0;
        // Wait 150ms for next chunk. If we don't get one, stream is over.
        uint8_t *item = (uint8_t *)xRingbufferReceive(audio_rb, &item_size, stream_active ? pdMS_TO_TICKS(150) : portMAX_DELAY);
        
        if (item) {
            if (!amp_enabled) {
                gpio_set_level(AUDIO_AMP_GPIO, 1);
                amp_enabled = true;
                vTaskDelay(pdMS_TO_TICKS(100)); // Give amp plenty of time to stabilize
                // Prime the I2S pipeline with some silence so the first real samples aren't lost
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
            // Stream timed out. Flush the DMA buffers with silence to stop any repeating/looping audio.
            if (stream_active) {
                for (int i = 0; i < 4; i++) {
                    i2s_channel_write(tx_chan, silence, sizeof(silence), &w_bytes, portMAX_DELAY);
                }
                stream_active = false;
                has_leftover = false;
                
                // Turn off amp to stop scritchy sounds
                gpio_set_level(AUDIO_AMP_GPIO, 0);
                amp_enabled = false;
                ESP_LOGI(TAG, "Audio stream finished, DMA flushed, Amp OFF.");
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

// dog_audio_play_tone() removed — was dead code using clip_bark from audio_clips.h
// Barking now uses dog_audio_play_bark() (dogbark_audio_8bit.h) everywhere

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
    ESP_LOGI(TAG, "Playing paulbot boot sound...");
    dog_audio_play_8bit(paulbot_audio_8bit, paulbot_audio_8bit_len);
    ESP_LOGI(TAG, "Paulbot boot sound finished");
}

void dog_audio_play_bark(void) {
    ESP_LOGI(TAG, "Barking...");
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
    else if (strcmp(name, "random") == 0) dog_audio_play_random();
    else ESP_LOGW(TAG, "Unknown sound name: %s", name);
}


static void init_audio(void) {
    ESP_LOGI(TAG, "Initializing PDM Audio");
    gpio_config_t amp_conf = {
        .pin_bit_mask = (1ULL << AUDIO_AMP_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp_conf);
    gpio_set_level(AUDIO_AMP_GPIO, 0);

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
        .atten = ADC_ATTEN_DB_12,
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
                    int hold_time = 0;
                    bool reset_triggered = false;
                    while(gpio_get_level(io_num) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        hold_time += 100;
                        if (hold_time >= 4000 && hold_time < 7000) {
                            int countdown = 7 - (hold_time / 1000);
                            char msg[32];
                            snprintf(msg, sizeof(msg), "Hold to\nReset: %d", countdown);
                            dog_set_oled_text(msg, 200);
                        }
                        if (hold_time >= 7000) {
                            reset_triggered = true;
                            break;
                        }
                    }
                    if (reset_triggered) {
                        while(gpio_get_level(io_num) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        dog_set_oled_text("Triple click\nto confirm", 5000);
                        
                        int click_count = 0;
                        int timeout = 5000;
                        while(timeout > 0) {
                            if (gpio_get_level(io_num) == 0) {
                                click_count++;
                                while(gpio_get_level(io_num) == 0) {
                                    vTaskDelay(pdMS_TO_TICKS(50));
                                    timeout -= 50;
                                }
                                vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
                                if (click_count >= 3) break;
                            }
                            vTaskDelay(pdMS_TO_TICKS(50));
                            timeout -= 50;
                        }
                        
                        if (click_count >= 3) {
                            ESP_LOGW(TAG, "Factory Reset Confirmed!");
                            dog_set_oled_text("RESETTING\nWIFI...", 5000);
                            nvs_handle_t h;
                            if (nvs_open("wifi_creds", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_erase_all(h);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            esp_restart();
                        } else {
                            dog_dismiss_oled();
                            dog_set_oled_text("Cancelled", 2000);
                        }
                        xQueueReset(button_evt_queue);
                    } else {
                        dog_dismiss_oled();
                        if (hold_time < 4000) {
                            eye_mood = (eye_mood + 1) % 4;
                            dog_audio_play_random(); // Play random sound on button press
                        } else {
                            dog_set_oled_text("Cancelled", 2000);
                        }
                    }
                } else {
                    force_blink = true;
                    dog_dismiss_oled();
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
