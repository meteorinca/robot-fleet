// main/main.c  —  CarBot
// ============================================================================
//  ESP32-C3 CarBot firmware entry point.
//  Hardware: L298N dual motor (steering + RWD), HC-SR04 ultrasonic,
//            0.96" OLED, passive buzzer, built-in LED, OTA WiFi.
// ============================================================================
#include "config.h"

#include "buzzer.h"
#include "led.h"
#include "motor.h"

#include "wifi_mgr.h"
#include "timekeep.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "ultrasonic.h"
#include "oled.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/queue.h"


bool g_btn1_state = false;
bool g_btn2_state = false;

bool g_led_direct_mode = false;  // CarBot: direct mode off by default (motors use those pins)

static void button_task(void *arg) {
    bool last_btn1 = false;
    bool last_btn2 = false;

    // Boot-button WiFi-reset state machine
    typedef enum {
        BOOT_IDLE,
        BOOT_HOLDING,
        BOOT_CONFIRMING,
    } boot_btn_state_t;
    boot_btn_state_t boot_state = BOOT_IDLE;
    uint32_t boot_hold_start = 0;
    bool last_boot_btn = false;

    while (1) {
        bool btn1 = (gpio_get_level(BTN_BOOT_GPIO) == 0);  // use boot btn as btn1 proxy
        bool btn2 = false;  // BTN_1/BTN_2 GPIOs shared with motor pins — not usable simultaneously

        g_btn1_state = btn1;
        g_btn2_state = btn2;

        uint32_t now = esp_log_timestamp();

        if (btn1 && !last_btn1) {
            buzzer_play_tone(800, 20);
        }

        // Boot button: 7-second hold → WiFi reset
        bool boot_raw = (gpio_get_level(BTN_BOOT_GPIO) == 0);
        uint32_t boot_now = esp_log_timestamp();

        switch (boot_state) {
            case BOOT_IDLE:
                if (boot_raw && !last_boot_btn) {
                    boot_hold_start = boot_now;
                    boot_state = BOOT_HOLDING;
                    ESP_LOGI("BTN", "Boot btn held — starting 7s WiFi reset countdown");
                }
                break;

            case BOOT_HOLDING:
                if (!boot_raw) {
                    // Short tap → show IP on OLED
                    ESP_LOGI("BTN", "Boot btn short tap → Show IP");
                    oled_set_mode(OLED_MODE_SHOW_IP);
                    boot_state = BOOT_IDLE;
                } else {
                    uint32_t held_ms = boot_now - boot_hold_start;
                    int held_s = (int)(held_ms / 1000);
                    if (held_s >= 1 && held_s < 7) {
                        char hint[32];
                        snprintf(hint, sizeof(hint), "Hold...%ds", 7 - held_s);
                        oled_set_text(hint, 800);
                    }
                    if (held_ms >= 7000) {
                        buzzer_play_tone(600, 300);
                        oled_set_mode(OLED_MODE_WIFI_RESET_CONFIRM);
                        boot_state = BOOT_CONFIRMING;
                        ESP_LOGW("BTN", "Boot btn held 7s — showing WiFi reset confirmation");
                    }
                }
                break;

            case BOOT_CONFIRMING:
                if (oled_get_mode() != OLED_MODE_WIFI_RESET_CONFIRM) {
                    boot_state = BOOT_IDLE;
                } else if (boot_raw && !last_boot_btn) {
                    ESP_LOGW("BTN", "WiFi reset CONFIRMED — erasing credentials and restarting");
                    oled_set_text("Resetting...", 5000);
                    vTaskDelay(pdMS_TO_TICKS(800));
                    wifi_forget_all();
                }
                break;
        }
        last_boot_btn = boot_raw;
        last_btn1 = btn1;
        last_btn2 = btn2;

        vTaskDelay(pdMS_TO_TICKS(30));
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
    buzzer_init();
    buzzer_demo_startup();
    motor_init();
    ultrasonic_init();
    oled_init();

    // Boot button only (motor pins own BTN_1/BTN_2 GPIOs)
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_BOOT_GPIO),
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

    ESP_LOGI("MAIN", "CarBot ready — v%s — %s.local:%d",
             FW_VERSION, MDNS_HOSTNAME, WEB_SERVER_PORT);
}
