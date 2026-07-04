import sys

path = 'oled.c'
with open(path, 'r', encoding='utf-8') as f:
    src = f.read()

start_marker = '        } else if (s_oled_mode == OLED_MODE_HEARTBEAT) {'
end_marker   = '            vTaskDelay(pdMS_TO_TICKS(30));\r\n            continue;\r\n'

si = src.find(start_marker)
ei = src.find(end_marker, si)
if si == -1 or ei == -1:
    # Try LF-only
    end_marker = end_marker.replace('\r\n', '\n')
    ei = src.find(end_marker, si)
if si == -1 or ei == -1:
    print('MARKERS NOT FOUND'); sys.exit(1)

end_idx = ei + len(end_marker)
print(f'Replacing chars {si}..{end_idx} ({end_idx-si} bytes)')

NEW_BLOCK = r"""        } else if (s_oled_mode == OLED_MODE_HEARTBEAT) {
            // \u2500\u2500 Heartbeat (~75 BPM lub-dub) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            // Frame time: 20ms.  Cycle: 40 frames = 800ms (~75 BPM).
            //  Frame  0: LUB -- heart scale snaps to 1.35x, buzzer 220Hz 35ms
            //  Frame  5: ease back to 1.0x
            //  Frame 10: DUB -- heart scale snaps to 1.18x, buzzer 160Hz 55ms
            //  Frame 15+: ease back, rest until frame 40 -> repeat
            //
            // Heart is drawn row-by-row via a bitmapped half-width table instead
            // of a nested float parametric loop -- much faster, solid coverage.
            //
            // EKG uses an unsigned scroll counter so (pos % period) is always
            // non-negative, fixing the broken-graph signed-modulo bug.

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

            // \u2500\u2500 Beat timing \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            beat_frame++;
            if (beat_frame >= 40) beat_frame = 0;

            float scale_target = 1.0f;
            if      (beat_frame < 5)  scale_target = 1.35f;
            else if (beat_frame < 10) scale_target = 1.0f;
            else if (beat_frame < 15) scale_target = 1.18f;

            // Spring-damper: snappy attack, silky release
            float spring = (scale_target - heart_scale) * 0.55f;
            scale_vel = scale_vel * 0.3f + spring;
            heart_scale += scale_vel;
            if (heart_scale < 0.85f) heart_scale = 0.85f;
            if (heart_scale > 1.42f) heart_scale = 1.42f;

            // Buzzer fires exactly once per beat event (flag prevents repeat)
            if (beat_frame == 0  && !snd_lub) { buzzer_play_tone(220, 35); snd_lub = true; }
            if (beat_frame == 1)                snd_lub = false;
            if (beat_frame == 10 && !snd_dub) { buzzer_play_tone(160, 55); snd_dub = true; }
            if (beat_frame == 11)               snd_dub = false;

            // \u2500\u2500 Heart shape (bitmapped row-fill) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            // Per-row half-widths for a base heart of radius ~12px.
            // Index 0 = topmost row (dy = -12 from center). 28 rows total.
            // Negative entries = no pixels (pointed tip at the bottom).
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

            // \u2500\u2500 Pulsing ring (visible during lub and dub expansion) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            if ((beat_frame < 8) || (beat_frame >= 10 && beat_frame < 18)) {
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

            // \u2500\u2500 BPM label \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            draw_text(44, 54, "75 BPM", 1);

            // \u2500\u2500 EKG trace (scrolls left, unsigned counter = no negative modulo) \u2500\u2500\u2500\u2500\u2500
            ekg_pos += 2;
            #define HB_EKG_PERIOD 64
            for (int x = 0; x < OLED_WIDTH; x++) {
                int ph = (int)((ekg_pos + (uint32_t)x) % HB_EKG_PERIOD);
                int oy = 0;

                // P wave: smooth gentle bump
                if      (ph >= 8  && ph < 12) oy = -(ph - 8);
                else if (ph >= 12 && ph < 16) oy = -(16 - ph);

                // QRS complex: sharp spike -- Q dip, tall R, S recovery
                else if (ph == 22) oy =  2;
                else if (ph == 23) oy =  3;
                else if (ph == 24) oy =  0;
                else if (ph == 25) oy = -10;
                else if (ph == 26) oy = -16;
                else if (ph == 27) oy = -10;
                else if (ph == 28) oy =  4;
                else if (ph == 29) oy =  4;
                else if (ph == 30) oy =  2;
                else if (ph == 31) oy =  0;

                // T wave: rounded bump
                else if (ph >= 38 && ph < 42) oy = -(ph - 38);
                else if (ph >= 42 && ph < 46) oy = -(46 - ph);

                int py = 50 + oy;
                if (py < 50) py = 50;
                if (py >= OLED_HEIGHT) py = OLED_HEIGHT - 1;
                draw_pixel(x, py, 1);
                if (py + 1 < OLED_HEIGHT) draw_pixel(x, py + 1, 1);
            }
            #undef HB_EKG_PERIOD

            oled_send_buffer();
            vTaskDelay(pdMS_TO_TICKS(20)); // 50fps, 40 frames = 800ms cycle (~75 BPM)
            continue;
"""

# Preserve original line endings: detect CRLF or LF
if '\r\n' in src:
    NEW_BLOCK = NEW_BLOCK.replace('\n', '\r\n')

result = src[:si] + NEW_BLOCK + src[end_idx:]
with open(path, 'w', encoding='utf-8') as f:
    f.write(result)
print('SUCCESS - file written')
