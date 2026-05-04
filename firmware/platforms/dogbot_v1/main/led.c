#include "led.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <math.h>

static bool g_led_state = false;
static volatile int64_t s_last_manual_time = -10000000LL; // Initialize far in the past

// Passed in from main so we don't need a global event group
static EventGroupHandle_t s_wifi_events;
static EventBits_t        s_connected_bit;

#define LED_LEDC_TIMER      LEDC_TIMER_1
#define LED_LEDC_CHANNEL    LEDC_CHANNEL_2

void led_init(void) {
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL);
}

void led_register_manual_control(void) {
    s_last_manual_time = esp_timer_get_time();
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

#include "ota_mgr.h"

// ── Heartbeat task ──
static void led_heartbeat_task(void *pvParameters) {
    uint32_t max_duty = 8191;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        // ── OTA Visual Feedback ──
        if (g_ota_state == OTA_STATE_ACTIVE) {
            // Rapid strobe during flashing
            bool strobe = (esp_timer_get_time() / 50000) % 2; // 50ms toggle
            led_set(strobe);
            continue;
        } else if (g_ota_state == OTA_STATE_SUCCESS) {
            // Solid ON for success
            led_set(true);
            continue;
        } else if (g_ota_state == OTA_STATE_FAILED) {
            // Very fast panic blink
            bool panic = (esp_timer_get_time() / 100000) % 2; // 100ms toggle
            led_set(panic);
            continue;
        }

        if (esp_timer_get_time() - s_last_manual_time < LED_MANUAL_OVERRIDE_US) {
            continue;
        }

        bool connected = (xEventGroupGetBits(s_wifi_events) & s_connected_bit) != 0;

        if (connected) {
            // Slow breathing pattern
            float t = esp_timer_get_time() / 1000000.0f;
            // 0.3 Hz breathing -> full cycle every ~3.3 seconds
            float breathe = (sinf(t * 3.14159f * 0.6f) + 1.0f) / 2.0f;
            
            // Gamma correction / squaring for more natural LED fade
            breathe = breathe * breathe;

            uint32_t duty = (uint32_t)(breathe * max_duty);
            if (LED_ACTIVE_LOW) duty = max_duty - duty;
            
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL);
        } else {
            // Disconnected pattern: double-pulse heartbeat
            int cycle_ms = (esp_timer_get_time() / 1000) % 1350;
            bool on = false;
            if (cycle_ms < 100) {
                on = true;
            } else if (cycle_ms < 250) {
                on = false;
            } else if (cycle_ms < 350) {
                on = true;
            } else {
                on = false;
            }
            led_set(on);
        }
    }
}

void led_start_heartbeat(EventGroupHandle_t wifi_events, EventBits_t connected_bit) {
    s_wifi_events   = wifi_events;
    s_connected_bit = connected_bit;
    xTaskCreate(led_heartbeat_task, "led_hb", 2048, NULL, 3, NULL);
}
