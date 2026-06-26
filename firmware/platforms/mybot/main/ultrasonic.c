#include "ultrasonic.h"
#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "ws2812.h"
#include "webserver.h" // For sse_broadcast_tts
#include <stdio.h>

#define MAX_DISTANCE_CM 200
#define TIMEOUT_US (MAX_DISTANCE_CM * 58)

static bool s_ultrasonic_active = false;
static const char *TAG = "ULTRASONIC";

void ultrasonic_set_active(bool active) {
    s_ultrasonic_active = active;
    if (!active) {
        ws2812_clear();
        ws2812_show();
    }
}

bool ultrasonic_is_active(void) {
    return s_ultrasonic_active;
}

// Simple HSV to RGB (Hue 0-360)
static uint32_t hsv_to_rgb_simple(float h, float s, float v) {
    int i = (int)(h / 60.0f);
    float f = (h / 60.0f) - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
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

// Map distance to NeoPixels. <5cm = 10 LEDs, >50cm = 1 LED.
// Spectrum: 5cm (Red, Hue 0) to 50cm (Blue, Hue 240)
static void update_neopixels(float distance) {
    int num_leds = 1;
    if (distance < 5.0) num_leds = 10;
    else if (distance >= 50.0) num_leds = 1;
    else {
        num_leds = 10 - (int)((distance - 5.0) / 5.0);
    }
    
    float hue = 0.0f;
    if (distance >= 50.0) hue = 240.0f;
    else if (distance > 5.0) hue = 240.0f * ((distance - 5.0) / 45.0f);
    
    uint32_t rgb = hsv_to_rgb_simple(hue, 1.0f, 1.0f);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;

    ws2812_clear();
    for (int i = 0; i < num_leds && i < WS2812_NUM_LEDS; i++) {
        ws2812_set_pixel(i, r, g, b);
    }
    ws2812_show();
}

static void ultrasonic_task(void *pvParameters) {
    gpio_reset_pin(TRIG_PIN);
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(TRIG_PIN, 0);

    gpio_reset_pin(ECHO_PIN);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);

    while (1) {
        if (!s_ultrasonic_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        gpio_set_level(TRIG_PIN, 1);
        esp_rom_delay_us(10);
        gpio_set_level(TRIG_PIN, 0);

        int64_t start_wait = esp_timer_get_time();
        while (gpio_get_level(ECHO_PIN) == 0 && (esp_timer_get_time() - start_wait) < TIMEOUT_US) {}

        if (gpio_get_level(ECHO_PIN) == 1) {
            int64_t start = esp_timer_get_time();
            while (gpio_get_level(ECHO_PIN) == 1 && (esp_timer_get_time() - start) < TIMEOUT_US) {}
            int64_t duration = esp_timer_get_time() - start;
            
            float distance = duration / 58.0f;
            update_neopixels(distance);
            
            char sse_data[128];
            snprintf(sse_data, sizeof(sse_data), "{\"type\":\"us\",\"dist\":%.1f}", distance);
            // We use sse_broadcast_tts as a general pipe for now
            sse_broadcast_tts(sse_data); 
        } else {
            // timeout
            update_neopixels(50.0f);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100)); // 10Hz is smooth enough
    }
}

void ultrasonic_init(void) {
    xTaskCreate(ultrasonic_task, "ultrasonic", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Ultrasonic task started");
}
