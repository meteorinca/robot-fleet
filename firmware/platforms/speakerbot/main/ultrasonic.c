#include "ultrasonic.h"
#include "board_config.h"

#if defined(ENABLE_ULTRASONIC) && ENABLE_ULTRASONIC


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>

#define MAX_DISTANCE_CM 200
#define TIMEOUT_US (MAX_DISTANCE_CM * 58)

static bool s_ultrasonic_active = false;
static float s_distance = 0.0f;
static const char *TAG = "ULTRASONIC";

void ultrasonic_set_active(bool active) {
    s_ultrasonic_active = active;
}

bool ultrasonic_is_active(void) {
    return s_ultrasonic_active;
}

float ultrasonic_get_distance(void) {
    return s_distance;
}

static void ultrasonic_task(void *pvParameters) {
#if defined(TRIG_PIN) && defined(ECHO_PIN)
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
            s_distance = distance;
        }
        
        vTaskDelay(pdMS_TO_TICKS(30)); // ~33Hz
    }
#else
    ESP_LOGW(TAG, "TRIG_PIN or ECHO_PIN not defined in board_config.h");
    vTaskDelete(NULL);
#endif
}

void ultrasonic_init(void) {
    xTaskCreate(ultrasonic_task, "ultrasonic", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Ultrasonic task started");
}

#endif // ENABLE_ULTRASONIC
