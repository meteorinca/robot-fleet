// main/motor.c
// ============================================================================
//  CarBot L298N Dual Motor Driver
//
//  Motor A = Steering (open-loop timed control)
//  Motor B = Drive (RWD)
//
//  PWM is done with ESP32 LEDC peripheral.
//  IN1/IN2 and IN3/IN4 are also PWM-capable; the "active" direction pin
//  carries the PWM duty, the "inactive" pin is held LOW.
//
//  Open-loop steering logic:
//    We track an estimated position (0-100, 50=center).
//    When motor_steer_set(target) is called, we compute the direction and
//    a proportional pulse duration = |delta| * STEER_MS_PER_UNIT ms,
//    apply the motor, then stop.  This runs in a dedicated FreeRTOS task so
//    it is non-blocking.
// ============================================================================

#include "motor.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

#define TAG "MOTOR"

// ── LEDC resolution and max duty ─────────────────────────────────────────────
#define LEDC_MAX_DUTY       ((1 << 10) - 1)  // 1023 for 10-bit

// ── ms per unit of steering position (0-100 scale) ───────────────────────────
// STEER_FULL_TURN_MS covers 0->100 (or 100->0), so per-unit = /100
#define STEER_MS_PER_UNIT   (STEER_FULL_TURN_MS / 50)   // 50 units from center to full lock

// ── Internal state ────────────────────────────────────────────────────────────
static volatile uint8_t  s_steer_pos   = 50;   // estimated position 0-100
static volatile int8_t   s_throttle    = 0;     // last throttle command -100 to +100
static volatile bool     s_braking     = false;

// ── Steering task queue ───────────────────────────────────────────────────────
typedef struct {
    uint8_t target;     // 0-100
} steer_cmd_t;

static QueueHandle_t s_steer_queue = NULL;

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint32_t pct_to_duty(uint8_t pct) {
    if (pct == 0) return 0;
    if (pct >= 100) return LEDC_MAX_DUTY;
    return ((uint32_t)pct * LEDC_MAX_DUTY) / 100;
}

static void ledc_set(ledc_channel_t ch, uint32_t duty) {
    ledc_set_duty(MOTOR_LEDC_MODE, ch, duty);
    ledc_update_duty(MOTOR_LEDC_MODE, ch);
}

// ── Drive motor (Motor B) ─────────────────────────────────────────────────────
void motor_drive_forward(uint8_t speed_pct) {
    // IN3 = PWM, IN4 = LOW
    uint32_t duty = pct_to_duty(speed_pct);
    gpio_set_level(MOTOR_DRIVE_IN4, 0);
    ledc_set(MOTOR_LEDC_CH_DRIVE, duty);
    s_throttle = (int8_t)speed_pct;
    s_braking  = false;
    ESP_LOGD(TAG, "Drive FWD %d%% (duty=%lu)", speed_pct, duty);
}

void motor_drive_backward(uint8_t speed_pct) {
    // IN3 = LOW, IN4 = PWM via manual toggle (LEDC ch drives IN3, so swap)
    // Since LEDC is wired to IN3, for reverse we PWM IN4 and keep IN3 LOW.
    // We drive IN4 via GPIO toggling at the requested duty in a simple way:
    // Set LEDC duty to 0 (IN3 off), then PWM IN4 directly via LEDC ch drive
    // redirect — actually we use LEDC on IN4 channel.
    // Because LEDC_CH_DRIVE is configured to IN3 GPIO, for reverse we need
    // to manually set IN3 low and pulse IN4. We'll use a software approach:
    // set LEDC duty=0, then do gpio direct for IN4 at requested pct.
    //
    // SIMPLEST APPROACH: for backward, idle the LEDC channel (duty=0 on IN3)
    // and set IN4 HIGH for full reverse, or use a second LEDC channel.
    // Since we have LEDC_CHANNEL_4 free, we configure it dynamically on IN4.
    ledc_set(MOTOR_LEDC_CH_DRIVE, 0);
    gpio_set_level(MOTOR_DRIVE_IN3, 0);

    // Configure LEDC channel 4 to drive IN4 for reverse
    ledc_channel_config_t ch4 = {
        .gpio_num   = MOTOR_DRIVE_IN4,
        .speed_mode = MOTOR_LEDC_MODE,
        .channel    = LEDC_CHANNEL_4,
        .timer_sel  = MOTOR_LEDC_TIMER,
        .duty       = pct_to_duty(speed_pct),
        .hpoint     = 0,
    };
    ledc_channel_config(&ch4);
    ledc_set_duty(MOTOR_LEDC_MODE, LEDC_CHANNEL_4, pct_to_duty(speed_pct));
    ledc_update_duty(MOTOR_LEDC_MODE, LEDC_CHANNEL_4);

    s_throttle = -(int8_t)speed_pct;
    s_braking  = false;
    ESP_LOGD(TAG, "Drive BWD %d%%", speed_pct);
}

void motor_drive_brake(void) {
    // Both IN3+IN4 HIGH = active brake
    ledc_set(MOTOR_LEDC_CH_DRIVE, LEDC_MAX_DUTY);
    gpio_set_level(MOTOR_DRIVE_IN4, 1);
    s_throttle = 0;
    s_braking  = true;
    ESP_LOGD(TAG, "Drive BRAKE");
}

void motor_drive_coast(void) {
    // Both IN3+IN4 LOW = coast / free-wheel
    ledc_set(MOTOR_LEDC_CH_DRIVE, 0);
    gpio_set_level(MOTOR_DRIVE_IN4, 0);
    s_throttle = 0;
    s_braking  = false;
    ESP_LOGD(TAG, "Drive COAST");
}

// ── Steering motor (Motor A) — raw direction ──────────────────────────────────
void motor_steer_raw(bool right, uint8_t speed_pct) {
    uint32_t duty = pct_to_duty(speed_pct);
    if (right) {
        // IN1 = PWM, IN2 = LOW
        gpio_set_level(MOTOR_STEER_IN2, 0);
        ledc_set(MOTOR_LEDC_CH_STEER, duty);
    } else {
        // IN1 = LOW, IN2 = PWM via LEDC channel 5
        ledc_set(MOTOR_LEDC_CH_STEER, 0);
        gpio_set_level(MOTOR_STEER_IN1, 0);
        ledc_channel_config_t ch5 = {
            .gpio_num   = MOTOR_STEER_IN2,
            .speed_mode = MOTOR_LEDC_MODE,
            .channel    = LEDC_CHANNEL_5,
            .timer_sel  = MOTOR_LEDC_TIMER,
            .duty       = duty,
            .hpoint     = 0,
        };
        ledc_channel_config(&ch5);
        ledc_update_duty(MOTOR_LEDC_MODE, LEDC_CHANNEL_5);
    }
}

void motor_steer_stop(void) {
    ledc_set(MOTOR_LEDC_CH_STEER, 0);
    gpio_set_level(MOTOR_STEER_IN1, 0);
    gpio_set_level(MOTOR_STEER_IN2, 0);
    ESP_LOGD(TAG, "Steer STOP at pos=%d", s_steer_pos);
}

void motor_steer_reset_center(void) {
    s_steer_pos = 50;
    ESP_LOGI(TAG, "Steer position reset to center (50)");
}

uint8_t motor_steer_get_pos(void) {
    return s_steer_pos;
}

// ── Steering task ─────────────────────────────────────────────────────────────
static void steer_task(void *arg) {
    steer_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_steer_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            uint8_t target = cmd.target;
            uint8_t current = s_steer_pos;

            if (abs((int)target - (int)current) <= STEER_DEADBAND) {
                ESP_LOGD(TAG, "Steer: deadband skip (cur=%d tgt=%d)", current, target);
                continue;
            }

            bool going_right = (target > current);
            uint8_t delta = going_right ? (target - current) : (current - target);

            // Proportional pulse: delta units × ms-per-unit
            uint32_t pulse_ms = (uint32_t)delta * STEER_MS_PER_UNIT;
            if (pulse_ms < 20) pulse_ms = 20;   // minimum pulse

            ESP_LOGI(TAG, "Steer: %d→%d (%s) for %lums",
                     current, target, going_right ? "RIGHT" : "LEFT", pulse_ms);

            motor_steer_raw(going_right, 80);   // 80% speed for reliable movement
            vTaskDelay(pdMS_TO_TICKS(pulse_ms));
            motor_steer_stop();

            // Update estimated position
            s_steer_pos = target;
        }
    }
}

// ── motor_steer_set: queue a target position ──────────────────────────────────
void motor_steer_set(uint8_t target_pos) {
    if (target_pos > 100) target_pos = 100;
    if (!s_steer_queue) return;

    // Drain any pending command (always use latest)
    steer_cmd_t dummy;
    xQueueReceive(s_steer_queue, &dummy, 0);

    steer_cmd_t cmd = { .target = target_pos };
    xQueueSend(s_steer_queue, &cmd, pdMS_TO_TICKS(10));
}

// ── Combined set ──────────────────────────────────────────────────────────────
void motor_set(int8_t throttle, uint8_t steer) {
    // Throttle: -100 to +100
    if (throttle > 0) {
        motor_drive_forward((uint8_t)throttle);
    } else if (throttle < 0) {
        motor_drive_backward((uint8_t)(-throttle));
    } else {
        motor_drive_brake();
    }
    // Steering: 0-100
    motor_steer_set(steer);
}

// ── Status ────────────────────────────────────────────────────────────────────
int8_t motor_drive_get_throttle(void) { return s_throttle; }
bool   motor_is_braking(void)         { return s_braking; }

// ── Initialization ────────────────────────────────────────────────────────────
void motor_init(void) {
    ESP_LOGI(TAG, "Motor init — L298N dual motor driver");

    // ── GPIO config for direction pins ────────────────────────────────────────
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << MOTOR_DRIVE_IN4) | (1ULL << MOTOR_STEER_IN1) | (1ULL << MOTOR_STEER_IN2),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(MOTOR_DRIVE_IN4, 0);
    gpio_set_level(MOTOR_STEER_IN1, 0);
    gpio_set_level(MOTOR_STEER_IN2, 0);

    // ── LEDC timer ────────────────────────────────────────────────────────────
    ledc_timer_config_t timer = {
        .speed_mode      = MOTOR_LEDC_MODE,
        .timer_num       = MOTOR_LEDC_TIMER,
        .duty_resolution = MOTOR_LEDC_RESOLUTION,
        .freq_hz         = MOTOR_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    // ── LEDC channel for Drive IN3 (forward PWM) ──────────────────────────────
    ledc_channel_config_t drive_ch = {
        .gpio_num   = MOTOR_DRIVE_IN3,
        .speed_mode = MOTOR_LEDC_MODE,
        .channel    = MOTOR_LEDC_CH_DRIVE,
        .timer_sel  = MOTOR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&drive_ch);

    // ── LEDC channel for Steer IN1 (right PWM) ────────────────────────────────
    ledc_channel_config_t steer_ch = {
        .gpio_num   = MOTOR_STEER_IN1,
        .speed_mode = MOTOR_LEDC_MODE,
        .channel    = MOTOR_LEDC_CH_STEER,
        .timer_sel  = MOTOR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&steer_ch);

    // ── Start steering task ───────────────────────────────────────────────────
    s_steer_queue = xQueueCreate(2, sizeof(steer_cmd_t));
    xTaskCreate(steer_task, "steer_task", 2048, NULL, 5, NULL);

    // Estimated position starts at center
    s_steer_pos = 50;

    ESP_LOGI(TAG, "Motor init complete. Steer pos=50 (center)");
}
