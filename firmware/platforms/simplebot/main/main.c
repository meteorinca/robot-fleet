#include "config.h"

#ifdef WS2812_NUM_LEDS
#include "ws2812.h"
#else
#include "led.h"
#endif

#include "wifi_mgr.h"
#include "timekeep.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "servo.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/queue.h"



static void button_task(void *arg) {
    while (1) {
        if (gpio_get_level(BTN_BOOT_GPIO) == 0) {
            ESP_LOGI("BTN", "Boot button pressed -> Servo Hi");
            servo_quick_action(1, 40, 90);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (gpio_get_level(BTN_1_GPIO) == 0) {
            ESP_LOGI("BTN", "Button 1 pressed -> Servo 1 ON");
            servo_quick_action(1, POS1_ON, POS1_NEUTRAL);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (gpio_get_level(BTN_2_GPIO) == 0) {
            ESP_LOGI("BTN", "Button 2 pressed -> Servo 2 ON");
            servo_quick_action(2, POS2_ON, POS2_NEUTRAL);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    // NVS (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Peripherals
    led_init();
    servo_init();
    servo_worker_start();

    // Buttons
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO) | (1ULL << BTN_1_GPIO) | (1ULL << BTN_2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_conf);

    // WiFi (starts web server on connect)
    EventGroupHandle_t wifi_events = wifi_init();

    // Time sync
    timekeep_init();
    timekeep_start_scheduler();

    // Background tasks
    led_start_heartbeat(wifi_events, WIFI_CONNECTED_BIT);
    xTaskCreate(button_task, "btn_task", 3072, NULL, 5, NULL);

    ESP_LOGI("MAIN", "System ready — v%s — %s.local:%d",
             FW_VERSION, MDNS_HOSTNAME, WEB_SERVER_PORT);
}
