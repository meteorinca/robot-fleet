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

// Scale an 0x00RRGGBB colour by [0.0, 1.0]
static uint32_t scale_rgb(uint32_t rgb, float scale) {
    uint8_t r = (uint8_t)(((rgb >> 16) & 0xFF) * scale);
    uint8_t g = (uint8_t)(((rgb >>  8) & 0xFF) * scale);
    uint8_t b = (uint8_t)(((rgb >>  0) & 0xFF) * scale);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
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
}

void led_set(bool on) {
    strip_fill(on ? WS2812_COLOR_ACTION : 0);
}

void led_register_manual_control(void) {
    s_last_manual_time = esp_timer_get_time();
    // Flash blue briefly to acknowledge
    strip_fill(WS2812_COLOR_ACTION);
}

void led_action_set(bool state) {
    led_register_manual_control();
    g_state = state;
    strip_fill(state ? WS2812_COLOR_ACTION : 0);
    ESP_LOGI(TAG, "%s", state ? "ON (blue)" : "OFF");
}

void led_action_toggle(void) {
    led_action_set(!g_state);
}

// ── WS2812 extras ────────────────────────────────────────────────────────────

void ws2812_set_all(uint32_t rgb) {
    strip_fill(rgb);
}

void ws2812_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip || index < 0 || index >= WS2812_NUM_LEDS) return;
    led_strip_set_pixel(s_strip, index, r, g, b);
}

void ws2812_show(void) {
    if (s_strip) led_strip_refresh(s_strip);
}

void ws2812_clear(void) {
    if (s_strip) led_strip_clear(s_strip);
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
            // Pacifica-like wave (blue/light blue/purple)
            float t = esp_timer_get_time() / 1000000.0f;
            for(int i = 0; i < WS2812_NUM_LEDS; i++) {
                // Hue base ~0.65 (blue). Wobble between 0.5 (cyan) and 0.8 (purple)
                float hue = 0.65f + sinf(t * 1.5f + i * 0.5f) * 0.15f; 
                // Very low brightness (0.05 to 0.1)
                float val = 0.05f + (sinf(t * 2.0f - i * 0.8f) + 1.0f) * 0.025f; 
                uint32_t rgb = hsv_to_rgb(hue, 1.0f, val);
                uint8_t r = (rgb >> 16) & 0xFF;
                uint8_t g = (rgb >> 8) & 0xFF;
                uint8_t b = rgb & 0xFF;
                led_strip_set_pixel(s_strip, i, r, g, b);
            }
            led_strip_refresh(s_strip);
        } else {
            // Red double-pulse (no WiFi)
            int cycle_ms = (int)((esp_timer_get_time() / 1000) % 1350);
            bool on = (cycle_ms < 100) || (cycle_ms >= 150 && cycle_ms < 250);
            strip_fill(on ? WS2812_COLOR_DISCO : 0);
        }
    }
}

void led_start_heartbeat(EventGroupHandle_t wifi_events, EventBits_t connected_bit) {
    s_wifi_events   = wifi_events;
    s_connected_bit = connected_bit;
    xTaskCreate(ws2812_heartbeat_task, "ws2812_hb", 3072, NULL, 3, NULL);
}
