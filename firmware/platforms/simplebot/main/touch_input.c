#include "touch_input.h"

#if SOC_TOUCH_SENSOR_SUPPORTED

#include "config.h"
#include "servo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "TOUCH";

// ── Tuning ───────────────────────────────────────────────────────────────────
// Touch fires when raw reading is (baseline + threshold_pct%) above baseline.
// With long wires the baseline is much larger, so a percentage scales correctly.
// 4% is a good starting point; raise if you get false triggers, lower if unresponsive.
#ifndef TOUCH_THRESHOLD_PCT
#define TOUCH_THRESHOLD_PCT 4
#endif

// Number of EMA steps (each 20 ms) with NO triggering after the hardware
// settle. This lets the EMA converge to the true idle reading without
// accidentally latching any pad as "pressed" on startup.
// 250 steps × 20 ms = 5 seconds warm-up.
#define TOUCH_WARMUP_STEPS  250

typedef struct {
    touch_pad_t pad;
    int         servo;
    int         angle;
    int         neutral;
    uint32_t    baseline;
    bool        pressed;
} touch_map_t;

static void touch_read_task(void *pvParameters) {
    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);

    // TOUCH_HVOLT_ATTEN_0V gives the widest charge swing — more sensitive and
    // better for long wires where parasitic capacitance is high.
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_0V);

    touch_map_t pads[] = {
        { TOUCH_S1_ON,  1, POS1_ON,  POS1_NEUTRAL, 0, false },
        { TOUCH_S1_OFF, 1, POS1_OFF, POS1_NEUTRAL, 0, false },
        { TOUCH_S2_ON,  2, POS2_ON,  POS2_NEUTRAL, 0, false },
        { TOUCH_S2_OFF, 2, POS2_OFF, POS2_NEUTRAL, 0, false },
    };
    const int count = sizeof(pads) / sizeof(pads[0]);

    for (int i = 0; i < count; i++) touch_pad_config(pads[i].pad);
    touch_pad_fsm_start();

    // ── Hardware settle ───────────────────────────────────────────────────────
    // Long wires greatly increase the capacitance on each pad, so the touch FSM
    // takes much longer to produce stable readings after fsm_start(). 2 s is
    // enough in practice; do NOT read before this completes.
    ESP_LOGI(TAG, "Settling hardware (2s)...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ── Seed baseline ─────────────────────────────────────────────────────────
    // Read each pad once to seed the EMA. We read the same pad a few times and
    // take the minimum to avoid seeding with a transiently-high noise spike.
    for (int i = 0; i < count; i++) {
        uint32_t val, min_val = UINT32_MAX;
        for (int s = 0; s < 5; s++) {
            touch_pad_read_raw_data(pads[i].pad, &val);
            if (val < min_val) min_val = val;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        pads[i].baseline = min_val;
    }

    // ── Warm-up: EMA convergence with NO triggering ───────────────────────────
    // This is the critical phase. We run the Exponential Moving Average for
    // TOUCH_WARMUP_STEPS cycles without ever setting pressed=true or firing a
    // servo. This guarantees the baseline converges to the true idle value
    // regardless of where it started, breaking the "stuck-pressed on boot" bug.
    ESP_LOGI(TAG, "Touch warm-up (%d ms)...", TOUCH_WARMUP_STEPS * 20);
    for (int w = 0; w < TOUCH_WARMUP_STEPS; w++) {
        uint32_t val;
        for (int i = 0; i < count; i++) {
            touch_pad_read_raw_data(pads[i].pad, &val);
            // Fast EMA (alpha ≈ 0.5 per ~14 steps) to converge quickly
            pads[i].baseline = (pads[i].baseline * 7 + val) / 8;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "Touch ready — baselines: %lu %lu %lu %lu",
             (unsigned long)pads[0].baseline,
             (unsigned long)pads[1].baseline,
             (unsigned long)pads[2].baseline,
             (unsigned long)pads[3].baseline);

    // ── Poll loop ─────────────────────────────────────────────────────────────
    uint32_t raw;
    while (1) {
        for (int i = 0; i < count; i++) {
            touch_pad_read_raw_data(pads[i].pad, &raw);

            // Threshold scales with baseline — works for both short and long wires.
            uint32_t delta  = pads[i].baseline * TOUCH_THRESHOLD_PCT / 100;
            if (delta < 3000) delta = 3000;          // noise floor guard
            uint32_t thresh = pads[i].baseline + delta;

            if (raw > thresh) {
                if (!pads[i].pressed) {
                    pads[i].pressed = true;
                    ESP_LOGI(TAG, "Pad %d ON  raw=%lu base=%lu thr=%lu",
                             i, (unsigned long)raw,
                             (unsigned long)pads[i].baseline,
                             (unsigned long)thresh);
                    servo_quick_action(pads[i].servo, pads[i].angle, pads[i].neutral);
                }
                // While pressed: do NOT update baseline so it doesn't chase the touch
            } else {
                // Slow EMA drift-tracking when idle (temp/humidity changes over hours)
                pads[i].baseline = (pads[i].baseline * 63 + raw) / 64;

                if (pads[i].pressed && raw < thresh - (delta / 3)) {
                    // Hysteresis: must drop noticeably below threshold before releasing
                    pads[i].pressed = false;
                    ESP_LOGI(TAG, "Pad %d OFF raw=%lu base=%lu",
                             i, (unsigned long)raw, (unsigned long)pads[i].baseline);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void touch_input_start(void) {
    xTaskCreate(touch_read_task, "touch", 4096, NULL, 3, NULL);
}

#endif // SOC_TOUCH_SENSOR_SUPPORTED
