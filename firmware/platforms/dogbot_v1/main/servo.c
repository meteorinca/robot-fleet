#include "servo.h"
#include "config.h"
#include "led.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ── Static channel/GPIO table — built from board_config.h ───────────────────
//  SERVO_COUNT is defined in the active board header (2 or 4).
//  Only entries 0..SERVO_COUNT-1 are configured.

typedef struct {
    gpio_num_t    gpio;
    ledc_channel_t channel;
} servo_hw_t;

static const servo_hw_t s_hw[] = {
    { SERVO1_GPIO, LEDC_CH_SERVO1 },
    { SERVO2_GPIO, LEDC_CH_SERVO2 },
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
    POS2_NEUTRAL,
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

static QueueHandle_t s_servo_queue;

// ────────────────────────────────────────────────────────────────────────────
void servo_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };

    for (int i = 0; i < (int)HW_COUNT; i++) {
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

void servo_action_set(int servo, int angle) {
    led_register_manual_control();
    servo_set_angle(servo, angle);
    ESP_LOGI("SERVO", "Servo%d -> %d", servo, angle);
}

void servo_quick_action(int servo, int target_angle, int neutral_angle) {
    servo_cmd_t cmd = { servo, target_angle, neutral_angle };
    if (s_servo_queue) {
        xQueueSend(s_servo_queue, &cmd, pdMS_TO_TICKS(50));
    }
}

// Background task — processes queued servo commands without blocking HTTP
static void servo_worker_task(void *pvParameters) {
    servo_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_servo_queue, &cmd, portMAX_DELAY)) {
            servo_action_set(cmd.servo, cmd.target_angle);
            vTaskDelay(pdMS_TO_TICKS(SERVO_RETURN_MS));
            servo_action_set(cmd.servo, cmd.neutral_angle);
        }
    }
}

void servo_worker_start(void) {
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
