// main/motor.c
// ============================================================================
//  CamBot — L298N Motor Driver Implementation
//
//  The L298N has ENA/ENB jumpered HIGH, so speed is bang-bang (full power or
//  stop). Direction is set by the IN1-IN4 GPIO logic levels:
//
//    IN1=1, IN2=0  → OUT1/OUT2 forward  (steering A forward)
//    IN1=0, IN2=1  → OUT1/OUT2 reverse  (steering A reverse)
//    IN1=1, IN2=1  → BRAKE  (active braking — short-circuits motor)
//
//    IN3=1, IN4=0  → OUT3/OUT4 forward  (drive B forward)
//    IN3=0, IN4=1  → OUT3/OUT4 reverse  (drive B reverse)
//    IN3=1, IN4=1  → BRAKE  (active braking)
//
//  The signed value API (-100…+100) maps: positive → forward, negative → reverse,
//  zero → stop.  The magnitude is accepted but not yet used for PWM; extend
//  motor_drive_raw() to add LEDC PWM on ENA/ENB if needed later.
// ============================================================================
#include "motor.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "MOTOR";

static int s_steer = 0;
static int s_drive = 0;

// ── helpers ──────────────────────────────────────────────────────────────────
static inline void steer_pwm(int in1_pct, int in2_pct) {
    uint32_t d1 = (in1_pct * 255) / 100;
    uint32_t d2 = (in2_pct * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, d1);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, d2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static inline void drive_pwm(int in3_pct, int in4_pct) {
    uint32_t d3 = (in3_pct * 255) / 100;
    uint32_t d4 = (in4_pct * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, d3);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, d4);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
}

static void init_pwm_channel(gpio_num_t gpio, ledc_channel_t channel) {
    ledc_channel_config_t ledc_channel = {
        .channel    = channel,
        .duty       = 0,
        .gpio_num   = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);
}

// ── public API ───────────────────────────────────────────────────────────────
void motor_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .freq_hz          = 1000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    init_pwm_channel(MOTOR_STEER_IN1, LEDC_CHANNEL_0);
    init_pwm_channel(MOTOR_STEER_IN2, LEDC_CHANNEL_1);
    init_pwm_channel(MOTOR_DRIVE_IN3, LEDC_CHANNEL_2);
    init_pwm_channel(MOTOR_DRIVE_IN4, LEDC_CHANNEL_3);

    steer_pwm(0, 0);
    drive_pwm(0, 0);

    ESP_LOGI(TAG, "Motor driver initialised with PWM (IN1=%d IN2=%d IN3=%d IN4=%d)",
             MOTOR_STEER_IN1, MOTOR_STEER_IN2, MOTOR_DRIVE_IN3, MOTOR_DRIVE_IN4);
}

void motor_steer(int value) {
    // Clamp
    if (value >  100) value =  100;
    if (value < -100) value = -100;
    s_steer = value;

    if (value > 0)       steer_pwm(value, 0);   // left
    else if (value < 0)  steer_pwm(0, -value);  // right
    else                 steer_pwm(100, 100);   // brake (IN1=IN2=1 — active hold)

    ESP_LOGD(TAG, "Steer → %d", value);
}

void motor_drive(int value) {
    if (value >  100) value =  100;
    if (value < -100) value = -100;
    s_drive = value;

    if (value > 0)       drive_pwm(value, 0);   // forward
    else if (value < 0)  drive_pwm(0, -value);  // reverse
    else                 drive_pwm(100, 100);   // brake (IN3=IN4=1 — active hold)

    ESP_LOGD(TAG, "Drive → %d", value);
}

void motor_stop_all(void) {
    s_steer = 0;
    s_drive = 0;
    steer_pwm(100, 100);   // brake
    drive_pwm(100, 100);   // brake
    ESP_LOGI(TAG, "Emergency stop (brake)");
}

int motor_get_steer(void) { return s_steer; }
int motor_get_drive(void) { return s_drive; }
