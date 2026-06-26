// ws2812.c — WS2812 NeoPixel strip driver (RMT, ESP-IDF v5 led_strip API)
//
// Only compiled when WS2812_NUM_LEDS is defined in board_config.h.
// Exposes the same led_* API as led.c so main.c / webserver.c are unchanged.

#include "ws2812.h"
#include "config.h"
#include "ota_mgr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"          // espressif/led_strip component
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include "board_config.h"
#include "driver/ledc.h"

#define LED_LEDC_TIMER      LEDC_TIMER_1
#define LED_LEDC_CHANNEL    LEDC_CHANNEL_4


static const char *TAG = "WS2812";

static led_strip_handle_t s_strip = NULL;
static bool               g_state = false;
static volatile int64_t   s_last_manual_time = -10000000LL;
static EventGroupHandle_t s_wifi_events  = NULL;
static EventBits_t        s_connected_bit = 0;

// ── Internal helpers ─────────────────────────────────────────────────────────

// Pack 0x00RRGGBB into individual components and write to every pixel
static void strip_fill(uint32_t rgb) {
    if (!s_strip) return;
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >>  8) & 0xFF;
    uint8_t b = (rgb >>  0) & 0xFF;
    for (int i = 0; i < WS2812_NUM_LEDS; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
}



// Dope as hell HSV to RGB converter
static uint32_t hsv_to_rgb(float h, float s, float v) {
    int i = (int)(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    float r = 0, g = 0, b = 0;
    switch (i % 6) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        case 5: r = v, g = p, b = q; break;
    }
    return ((uint32_t)(r * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
}

// ── Public API (led.h compatible) ────────────────────────────────────────────

void led_init(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num            = WS2812_GPIO,
        .max_leds                  = WS2812_NUM_LEDS,
        .color_component_format    = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model                 = LED_MODEL_WS2812,
        .flags.invert_out          = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RMT_RES_HZ,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);   // all off at startup
    ESP_LOGI(TAG, "WS2812 strip ready: %d LEDs on GPIO %d", WS2812_NUM_LEDS, WS2812_GPIO);

    // Initialize the built-in GPIO LED with LEDC for breathing
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
    g_state = state;
    led_set(state);
    ESP_LOGI(TAG, "%s (built-in)", state ? "ON" : "OFF");
}

void led_action_toggle(void) {
    led_action_set(!g_state);
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

// ── WS2812 extras ────────────────────────────────────────────────────────────

void ws2812_set_all(uint32_t rgb) {
    s_last_manual_time = esp_timer_get_time();
    strip_fill(rgb);
}

void ws2812_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip || index < 0 || index >= WS2812_NUM_LEDS) return;
    s_last_manual_time = esp_timer_get_time();
    led_strip_set_pixel(s_strip, index, r, g, b);
}

void ws2812_show(void) {
    if (s_strip) led_strip_refresh(s_strip);
}

void ws2812_clear(void) {
    if (s_strip) {
        s_last_manual_time = esp_timer_get_time();
        led_strip_clear(s_strip);
    }
}

void ws2812_resume_heartbeat(void) {
    s_last_manual_time = -10000000LL;
}

// ── Heartbeat task ────────────────────────────────────────────────────────────
static void ws2812_heartbeat_task(void *pvParameters) {
    int ota_fail_ticks = 0;   // countdown for OTA_FAILED display (~5 s)

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));

        // ── OTA state takes priority over everything ──────────────────────
        ota_state_t ota = g_ota_state;

        if (ota == OTA_STATE_ACTIVE) {
            // Cyan rapid strobe ~10 Hz (50 ms on / 50 ms off cycling across LEDs)
            int64_t t_ms = esp_timer_get_time() / 1000;
            bool phase = (t_ms / 50) & 1;
            strip_fill(phase ? 0x000A0A : 0);    // dim cyan
            continue;
        }

        if (ota == OTA_STATE_SUCCESS) {
            // Green triple-flash then hold off (restart is imminent anyway)
            for (int flash = 0; flash < 3; flash++) {
                strip_fill(0x00200A);             // dim green
                vTaskDelay(pdMS_TO_TICKS(150));
                strip_fill(0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(500));       // brief pause before reboot
            continue;
        }

        if (ota == OTA_STATE_FAILED) {
            // Fast red blink for ~5 s then auto-revert to IDLE
            ota_fail_ticks++;
            int64_t t_ms = esp_timer_get_time() / 1000;
            bool phase = (t_ms / 100) & 1;       // 5 Hz blink
            strip_fill(phase ? 0x200000 : 0);
            if (ota_fail_ticks > 250) {           // 250 × 20 ms = 5 s
                g_ota_state   = OTA_STATE_IDLE;
                ota_fail_ticks = 0;
            }
            continue;
        }
        ota_fail_ticks = 0;   // reset on IDLE

        // ── Normal WiFi heartbeat ─────────────────────────────────────────
        // Manual override: suppress heartbeat for LED_MANUAL_OVERRIDE_US
        if (esp_timer_get_time() - s_last_manual_time < LED_MANUAL_OVERRIDE_US) {
            continue;
        }

        bool connected = s_wifi_events &&
                         (xEventGroupGetBits(s_wifi_events) & s_connected_bit) != 0;

        if (connected) {
            // Pacifica-like wave for NeoPixels
            float t = esp_timer_get_time() / 1000000.0f;
            for(int i = 0; i < WS2812_NUM_LEDS; i++) {
                float hue = 0.65f + sinf(t * 1.5f + i * 0.5f) * 0.15f; 
                float val = 0.05f + (sinf(t * 2.0f - i * 0.8f) + 1.0f) * 0.025f; 
                uint32_t rgb = hsv_to_rgb(hue, 1.0f, val);
                uint8_t r = (rgb >> 16) & 0xFF;
                uint8_t g = (rgb >> 8) & 0xFF;
                uint8_t b = rgb & 0xFF;
                led_strip_set_pixel(s_strip, i, r, g, b);
            }
            led_strip_refresh(s_strip);

            // Breathing for Built-in LED
            float breathe = (sinf(t * 3.14159f * 0.6f) + 1.0f) / 2.0f;
            breathe = breathe * breathe; // gamma correction
            uint32_t max_duty = 8191;
            uint32_t duty = (uint32_t)(breathe * max_duty);
            if (LED_ACTIVE_LOW) duty = max_duty - duty;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_LEDC_CHANNEL);
        } else {
            // Red double-pulse (no WiFi) for NeoPixels
            int cycle_ms = (int)((esp_timer_get_time() / 1000) % 1350);
            bool on = (cycle_ms < 100) || (cycle_ms >= 150 && cycle_ms < 250);
            strip_fill(on ? WS2812_COLOR_DISCO : 0);
            led_set(on);
        }
    }
}

void led_start_heartbeat(EventGroupHandle_t wifi_events, EventBits_t connected_bit) {
    s_wifi_events   = wifi_events;
    s_connected_bit = connected_bit;
    xTaskCreate(ws2812_heartbeat_task, "ws2812_hb", 3072, NULL, 3, NULL);
}
