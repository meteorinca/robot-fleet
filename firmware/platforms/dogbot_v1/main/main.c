#include "config.h"

#ifdef WS2812_NUM_LEDS
#include "ws2812.h"
#else
#include "led.h"
#endif

#ifdef DISP_MOSI_GPIO
#include "dog_peripherals.h"
#endif
#include "servo.h"
#include "dog_actions.h"
#include "wifi_mgr.h"
#include "timekeep.h"
#include "touch_input.h"
#include "nvs_flash.h"
#include "esp_log.h"

// rf.h is only compiled in when the board has an RF module (see main/CMakeLists.txt)
#ifdef RF_RX_GPIO
#include "rf.h"
#endif

#ifdef DISP_MOSI_GPIO
static void startup_audio_task(void *arg) {
    EventGroupHandle_t wifi_events = (EventGroupHandle_t)arg;
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    dog_audio_play_paulbot();
    vTaskDelete(NULL);
}
#endif

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

#ifdef DISP_MOSI_GPIO
    dog_peripherals_init();
#endif

    // Set all servos to neutral on boot
    for (int i = 1; i <= servo_count(); i++) {
        servo_set_angle(i, servo_neutral(i));
    }
    servo_worker_start();
    dog_actions_start();    // dog animation task (queues actions, non-blocking)

    // 433 MHz RF (only on boards that define RF_RX_GPIO)
#ifdef RF_RX_GPIO
    rf_module_init();
    rf_start_receiver();
#endif

    // WiFi (starts web server on connect)
    EventGroupHandle_t wifi_events = wifi_init();

    // Time sync
    timekeep_init();
    timekeep_start_scheduler();

    // Background tasks
    led_start_heartbeat(wifi_events, WIFI_CONNECTED_BIT);

#ifdef DISP_MOSI_GPIO
    // Play startup audio message after wifi connects
    xTaskCreate(startup_audio_task, "startup_audio", 4096, wifi_events, 5, NULL);
#endif

    // Touch pads (ESP32-S3 only — compiled away on C3 via SOC guard)
#if SOC_TOUCH_SENSOR_SUPPORTED
    touch_input_start();
#endif

    ESP_LOGI("MAIN", "System ready — v%s — %s.local:%d (%d servos)",
             FW_VERSION, MDNS_HOSTNAME, WEB_SERVER_PORT, servo_count());
}
