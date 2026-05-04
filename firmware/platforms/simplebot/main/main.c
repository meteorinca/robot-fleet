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

    // WiFi (starts web server on connect)
    EventGroupHandle_t wifi_events = wifi_init();

    // Time sync
    timekeep_init();
    timekeep_start_scheduler();

    // Background tasks
    led_start_heartbeat(wifi_events, WIFI_CONNECTED_BIT);

    ESP_LOGI("MAIN", "System ready — v%s — %s.local:%d",
             FW_VERSION, MDNS_HOSTNAME, WEB_SERVER_PORT);
}
