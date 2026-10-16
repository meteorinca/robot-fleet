#include "led.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_led_mutex;
static uint32_t s_last_duty = UINT32_MAX;

static bool g_led_state = false;
static volatile int64_t s_last_manual_time = -10000000LL; // Initialize far in the past

// Passed in from main so we don't need a global event group
static EventGroupHandle_t s_wifi_events;
static EventBits_t        s_connected_bit;

#define LED_LEDC_TIMER      LEDC_TIMER_1
#define LED_LEDC_CHANNEL    LEDC_CHANNEL_4

static void write_duty(uint32_t duty) {
    if (duty == s_last_duty) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL);
    s_last_duty = duty;
}

void led_init(void) {
    s_led_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_led_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LED_LEDC_TIMER,
        .duty_resolution  = LEDC_TIMER_13_BIT,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LED_LEDC_CHANNEL,
        .timer_sel      = LED_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LED_GPIO,
        .duty           = LED_ACTIVE_LOW ? 8191 : 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
    
    led_set(false);
}

void led_set(bool on) {
    uint32_t max_duty = 8191;
    uint32_t duty;
    if (LED_ACTIVE_LOW) {
        duty = on ? 0 : max_duty;
    } else {
        duty = on ? max_duty : 0;
    }
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    write_duty(duty);
    xSemaphoreGive(s_led_mutex);
}

void led_register_manual_control(void) {
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    s_last_manual_time = esp_timer_get_time();
    xSemaphoreGive(s_led_mutex);
}

void led_action_set(bool state) {
    led_register_manual_control();
    g_led_state = state;
    led_set(g_led_state);
    ESP_LOGI("LED", "%s", g_led_state ? "ON" : "OFF");
}

void led_action_toggle(void) {
    led_action_set(!g_led_state);
}

void led_blink(int count, int ms_period) {
    led_register_manual_control();
    for (int i = 0; i < count; i++) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(ms_period / 2));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(ms_period / 2));
    }
}

#include "ota_mgr.h"

// Integer breathing curve sampled at 25 Hz. LEDC generates the PWM;
// unchanged strobe/solid states do not rewrite the hardware registers.
static uint32_t breathe_duty(int64_t now_ms) {
    unsigned phase = now_ms % 3200;
    unsigned triangle = phase < 1600 ? phase : 3200 - phase;
    uint32_t x = triangle * 1024 / 1600;
    uint32_t smooth = x * x * (3072 - 2 * x) / (1024 * 1024);
    return smooth * 8191 / 1024;
}

static void led_heartbeat_task(void *pvParameters) {
    while (1) {
        int64_t now_us = esp_timer_get_time();
        int64_t now_ms = now_us / 1000;
        uint32_t brightness = 0;
        bool manual = false;
        xSemaphoreTake(s_led_mutex, portMAX_DELAY);
        if (g_ota_state == OTA_STATE_ACTIVE) {
            brightness = (now_ms / 50) % 2 ? 8191 : 0;
        } else if (g_ota_state == OTA_STATE_SUCCESS) {
            brightness = 8191;
        } else if (g_ota_state == OTA_STATE_FAILED) {
            brightness = (now_ms / 100) % 2 ? 8191 : 0;
        } else if (now_us - s_last_manual_time < LED_MANUAL_OVERRIDE_US) {
            manual = true;
        } else if (xEventGroupGetBits(s_wifi_events) & s_connected_bit) {
            brightness = breathe_duty(now_ms);
        } else {
            unsigned t = now_ms % 3000;
            brightness = (t < 40 || (t >= 120 && t < 160) ||
                          (t >= 1500 && t < 1540)) ? 8191 : 0;
        }
        if (!manual) write_duty(LED_ACTIVE_LOW ? 8191 - brightness : brightness);
        xSemaphoreGive(s_led_mutex);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void led_start_heartbeat(EventGroupHandle_t wifi_events, EventBits_t connected_bit) {
    s_wifi_events   = wifi_events;
    s_connected_bit = connected_bit;
    xTaskCreate(led_heartbeat_task, "led_hb", 2048, NULL, 3, NULL);
}
