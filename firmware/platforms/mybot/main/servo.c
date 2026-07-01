#include "servo.h"
#include "config.h"
#include "led.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include "esp_random.h"

// ── Static channel/GPIO table — built from board_config.h ───────────────────
//  SERVO_COUNT is defined in the active board header (2 or 4).
//  Only entries 0..SERVO_COUNT-1 are configured.

typedef struct {
    gpio_num_t    gpio;
    ledc_channel_t channel;
} servo_hw_t;

static const servo_hw_t s_hw[] = {
    { SERVO1_GPIO, LEDC_CH_SERVO1 },
#if SERVO_COUNT >= 2
    { SERVO2_GPIO, LEDC_CH_SERVO2 },
#endif
#if SERVO_COUNT >= 3
    { SERVO3_GPIO, LEDC_CH_SERVO3 },
#endif
#if SERVO_COUNT >= 4
    { SERVO4_GPIO, LEDC_CH_SERVO4 },
#endif
};
#define HW_COUNT  (sizeof(s_hw) / sizeof(s_hw[0]))

// ── Default positions table (1-indexed to match servo_num API) ──────────────
//  Neutral / On / Off values come from board_config.h
static const int s_neutral[] = {
    0,           // [0] unused (1-indexed API)
    POS1_NEUTRAL,
#if SERVO_COUNT >= 3
    POS3_NEUTRAL,
#endif
#if SERVO_COUNT >= 4
    POS4_NEUTRAL,
#endif
};

typedef struct {
    int servo;           // 1-based
    int target_angle;
    int neutral_angle;
} servo_cmd_t;

static QueueHandle_t    s_servo_queue;
static SemaphoreHandle_t s_ledc_mutex;          // guards all LEDC register writes
static int  s_current_angles[4];                // last commanded angle per servo
static bool s_servo_detached[4] = {true, true, true, true}; // physical detach state
static bool s_random_look_enabled = false;

void servo_set_random_look(bool enable) {
    s_random_look_enabled = enable;
    ESP_LOGI("SERVO", "Random look %s", enable ? "enabled" : "disabled");
}

// ────────────────────────────────────────────────────────────────────────────
void servo_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_USE_APB_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };

    for (int i = 0; i < (int)HW_COUNT; i++) {
        gpio_reset_pin(s_hw[i].gpio); // Vital for JTAG pins on ESP32-C3 (e.g. GPIO 5) to output PWM
        ch.channel  = s_hw[i].channel;
        ch.gpio_num = s_hw[i].gpio;
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }

    ESP_LOGI("SERVO", "Initialized %d servo(s)", (int)HW_COUNT);
}

// ────────────────────────────────────────────────────────────────────────────
void servo_set_angle(int servo_num, int angle) {
    if (servo_num < 1 || servo_num > (int)HW_COUNT) return;
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;
    uint32_t pulse_us = SERVO_MIN_PULSE_US
                      + (angle * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) / 180);
    uint32_t duty = (pulse_us * (1u << 14)) / 20000;
    ledc_channel_t ch = s_hw[servo_num - 1].channel;
    // Mutex prevents interleaved set_duty/update_duty if the HTTP handler
    // calls servo_action_set while the worker task is mid-move.
    xSemaphoreTake(s_ledc_mutex, portMAX_DELAY);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    xSemaphoreGive(s_ledc_mutex);
    s_servo_detached[servo_num - 1] = false;
}

void servo_action_set(int servo, int angle) {
    led_register_manual_control();
    servo_set_angle(servo, angle);
    if (servo >= 1 && servo <= (int)HW_COUNT) {
        s_current_angles[servo - 1] = angle;
    }
    ESP_LOGI("SERVO", "Servo%d -> %d", servo, angle);
}

void servo_quick_action(int servo, int target_angle, int neutral_angle) {
    servo_cmd_t cmd = { servo, target_angle, neutral_angle };
    if (s_servo_queue) {
        xQueueSend(s_servo_queue, &cmd, pdMS_TO_TICKS(50));
    }
}

// ────────────────────────────────────────────────────────────────────────────
void servo_detach(int servo_num) {
    if (servo_num < 1 || servo_num > (int)HW_COUNT) return;
    ledc_channel_t ch = s_hw[servo_num - 1].channel;
    xSemaphoreTake(s_ledc_mutex, portMAX_DELAY);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    xSemaphoreGive(s_ledc_mutex);
    s_servo_detached[servo_num - 1] = true;
}

void servo_move_stepped(int servo_num, int target, int ms_delay) {
    if (servo_num < 1 || servo_num > (int)HW_COUNT) return;
    int cur = s_current_angles[servo_num - 1];
    if (cur < 0 || cur > 180) cur = 90; // safety clamp

    // If the servo was detached, re-engage it at the last known angle before
    // stepping. Without this, the servo jumps from wherever it physically drifted
    // to the software-tracked angle at the first step — the root cause of jitter.
    // We write the position, wait one full 50Hz PWM frame (20ms) + a small settle
    // margin so the servo actually holds that position before we begin moving.
    if (s_servo_detached[servo_num - 1]) {
        servo_set_angle(servo_num, cur);
        vTaskDelay(pdMS_TO_TICKS(50)); // 20ms frame + 30ms settle
    }

    while (cur != target) {
        if (cur < target) cur++;
        else cur--;
        servo_set_angle(servo_num, cur);
        s_current_angles[servo_num - 1] = cur;
        vTaskDelay(pdMS_TO_TICKS(ms_delay));
    }
}

// Background task — processes queued servo commands without blocking HTTP
static void servo_worker_task(void *pvParameters) {
    servo_cmd_t cmd;
    
    // Boot sequence: detach (off) by default
    for (int i=0; i<HW_COUNT; i++) {
        int neut = servo_neutral(i + 1);
        s_current_angles[i] = neut;
        servo_detach(i + 1);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i=0; i<HW_COUNT; i++) {
        servo_detach(i + 1);
    }

    int random_delay_ticks = 20; // 2 seconds initial wait if enabled
    while (1) {
        if (xQueueReceive(s_servo_queue, &cmd, pdMS_TO_TICKS(100))) {
            // Pulse to target at "medium speed" (~10ms per degree)
            servo_move_stepped(cmd.servo, cmd.target_angle, 10);
            
            vTaskDelay(pdMS_TO_TICKS(200)); // brief hold at pulse peak
            
            // Go back to neutral at "medium speed"
            servo_move_stepped(cmd.servo, cmd.neutral_angle, 10);
            
            // Wait 1 second then turn off
            vTaskDelay(pdMS_TO_TICKS(1000));
            servo_detach(cmd.servo);
        } else {
            if (s_random_look_enabled) {
                if (random_delay_ticks <= 0) {
                    // Random angle between 60 and 120 (Not too wide)
                    int target_angle = 60 + (esp_random() % 61);
                    // Random speed 15 to 35 ms per degree (Not too fast, natural)
                    int speed = 15 + (esp_random() % 21);
                    
                    servo_move_stepped(1, target_angle, speed);
                    
                    // Wait random time between 1 to 4 seconds
                    random_delay_ticks = 10 + (esp_random() % 31);
                } else {
                    random_delay_ticks--;
                }
            }
        }
    }
}

void servo_worker_start(void) {
    s_ledc_mutex  = xSemaphoreCreateMutex();
    s_servo_queue = xQueueCreate(8, sizeof(servo_cmd_t));
    xTaskCreate(servo_worker_task, "servo_w", 4096, NULL, 4, NULL);
}

int servo_count(void) {
    return (int)HW_COUNT;
}

int servo_neutral(int servo_num) {
    if (servo_num < 1 || servo_num >= (int)(sizeof(s_neutral)/sizeof(s_neutral[0]))) return 90;
    return s_neutral[servo_num];
}

int servo_get_last_angle(int servo_num) {
    if (servo_num < 1 || servo_num > (int)HW_COUNT) return 90;
    return s_current_angles[servo_num - 1];
}
