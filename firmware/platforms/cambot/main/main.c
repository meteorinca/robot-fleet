#include "config.h"
#include "camera.h"
#include "led.h"
#include "motor.h"
#include "wifi_mgr.h"
#include "timekeep.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Boot button toggles the camera stream on/off
static void button_task(void *arg) {
    bool last_pressed = false;
    while (1) {
        bool pressed = (gpio_get_level(BTN_BOOT_GPIO) == 0);
        if (pressed && !last_pressed) {
            bool new_state = !camera_is_streaming();
            camera_set_streaming(new_state);
            led_blink(new_state ? 2 : 1, 100);
            ESP_LOGI("BTN", "Boot button: stream %s", new_state ? "ON" : "OFF");
        }
        last_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void) {
    // NVS (required by WiFi + camera driver)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // LED
    led_init();

    // Motor driver (L298N — steering + drive)
    motor_init();

    // Boot button
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_conf);

    // Camera (must be before WiFi — camera uses I2S/LCD peripherals)
    ESP_ERROR_CHECK(camera_init());

    // WiFi (starts web server on connect)
    EventGroupHandle_t wifi_events = wifi_init();

    // Time sync + scheduler
    timekeep_init();
    timekeep_start_scheduler();

    // Background tasks
    led_start_heartbeat(wifi_events, WIFI_CONNECTED_BIT);
    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);

    ESP_LOGI("MAIN", "CamBot ready — v%s — %s.local:%d",
             FW_VERSION, MDNS_HOSTNAME, WEB_SERVER_PORT);
    ESP_LOGI("MAIN", "Stream: OFF (press boot button or GET /cam_on to start)");
}
