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
#include "ultrasonic.h"
#include "oled.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/queue.h"


bool g_btn1_state = false;
bool g_btn2_state = false;

static void button_task(void *arg) {
    bool last_btn1 = false;
    bool last_btn2 = false;

    while (1) {
        bool btn1 = (gpio_get_level(BTN_1_GPIO) == 0);
        bool btn2 = (gpio_get_level(BTN_2_GPIO) == 0);
        
        g_btn1_state = btn1;
        g_btn2_state = btn2;
        
        oled_set_paddle_input(btn1, btn2);
        
        if (oled_get_mode() == OLED_MODE_NORMAL) {
            if (btn1 && !last_btn1) {
                eye_emotion_t emo = oled_get_emotion();
                emo = (emo + 1) % EYE_EMOTION_COUNT;
                oled_set_emotion(emo);
                
#ifdef WS2812_NUM_LEDS
                if (emo == EYE_EMOTION_NORMAL) ws2812_set_all(0x000000);
                else if (emo == EYE_EMOTION_MAD) ws2812_set_all(0xFF0000);
                else if (emo == EYE_EMOTION_SAD) ws2812_set_all(0x0000FF);
                else if (emo == EYE_EMOTION_SLEEPY) ws2812_set_all(0x00FF00);
                else if (emo == EYE_EMOTION_SURPRISED) ws2812_set_all(0xFFFF00);
                ws2812_show();
#endif
                ESP_LOGI("BTN", "Emotion forward to %d", emo);
            }
            if (btn2 && !last_btn2) {
                eye_emotion_t emo = oled_get_emotion();
                if (emo == 0) emo = EYE_EMOTION_COUNT - 1;
                else emo--;
                oled_set_emotion(emo);
                
#ifdef WS2812_NUM_LEDS
                if (emo == EYE_EMOTION_NORMAL) ws2812_set_all(0x000000);
                else if (emo == EYE_EMOTION_MAD) ws2812_set_all(0xFF0000);
                else if (emo == EYE_EMOTION_SAD) ws2812_set_all(0x0000FF);
                else if (emo == EYE_EMOTION_SLEEPY) ws2812_set_all(0x00FF00);
                else if (emo == EYE_EMOTION_SURPRISED) ws2812_set_all(0xFFFF00);
                ws2812_show();
#endif
                ESP_LOGI("BTN", "Emotion backward to %d", emo);
            }
        }
        
        last_btn1 = btn1;
        last_btn2 = btn2;

        if (gpio_get_level(BTN_BOOT_GPIO) == 0) {
            ESP_LOGI("BTN", "Boot button pressed -> Servo Hi");
            servo_quick_action(1, 40, 90);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(30)); // fast poll for games
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
    ultrasonic_init();
    oled_init();

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
