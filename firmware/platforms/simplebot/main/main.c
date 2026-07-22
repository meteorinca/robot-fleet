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



#if defined(HAS_OLED) || defined(OLED_SDA_PIN)
#include "oled.h"
#endif

static void button_task(void *arg) {
    while (1) {
        // ── Boot button: Servo Hi on short tap, WiFi reset on 7-second hold ────
        if (gpio_get_level(BTN_BOOT_GPIO) == 0) {
            int hold_time = 0;
            bool reset_triggered = false;
            while (gpio_get_level(BTN_BOOT_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                hold_time += 100;
                if (hold_time >= 4000 && hold_time < 7000) {
                    int countdown = 7 - (hold_time / 1000);
                    ESP_LOGW("BTN", "Hold to reset WiFi: %ds", countdown);
                    led_blink(1, 50);
#if defined(HAS_OLED) || defined(OLED_SDA_PIN)
                    char msg[32];
                    snprintf(msg, sizeof(msg), "Hold to\nReset: %d", countdown);
                    oled_set_text(msg, 200);
#endif
                }
                if (hold_time >= 7000) {
                    reset_triggered = true;
                    break;
                }
            }

            if (reset_triggered) {
                while (gpio_get_level(BTN_BOOT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                ESP_LOGW("BTN", "Release detected. Triple click BOOT button within 5s to confirm WiFi reset!");
#if defined(HAS_OLED) || defined(OLED_SDA_PIN)
                oled_set_text("Triple click\nto confirm", 5000);
#endif

                int click_count = 0;
                int timeout = 5000;
                while (timeout > 0) {
                    if (gpio_get_level(BTN_BOOT_GPIO) == 0) {
                        click_count++;
                        led_blink(2, 50);
                        while (gpio_get_level(BTN_BOOT_GPIO) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(50));
                            timeout -= 50;
                        }
                        vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
                        if (click_count >= 3) break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                    timeout -= 50;
                }

                if (click_count >= 3) {
                    ESP_LOGW("BTN", "WiFi reset CONFIRMED!");
                    led_blink(10, 50);
#if defined(HAS_OLED) || defined(OLED_SDA_PIN)
                    oled_set_text("RESETTING\nWIFI...", 5000);
#endif
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    wifi_forget_all(); // Does not return
                } else {
                    ESP_LOGI("BTN", "WiFi reset cancelled");
#if defined(HAS_OLED) || defined(OLED_SDA_PIN)
                    oled_set_text("", 0);
                    oled_set_text("Cancelled", 2000);
#endif
                }
            } else {
                if (hold_time < 4000) {
                    // Short tap: Servo Hi action
                    ESP_LOGI("BTN", "Boot button pressed -> Servo Hi");
                    servo_quick_action(1, 40, 90);
                } else {
                    ESP_LOGI("BTN", "WiFi reset cancelled");
#if defined(HAS_OLED) || defined(OLED_SDA_PIN)
                    oled_set_text("", 0);
                    oled_set_text("Cancelled", 2000);
#endif
                }
            }
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
