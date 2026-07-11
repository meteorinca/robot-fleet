#include "config.h"

#include "buzzer.h"
#include "led.h"

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

bool g_led_direct_mode = true;

static void button_task(void *arg) {
    bool last_btn1 = false;
    bool last_btn2 = false;
    uint32_t btn2_press_start = 0;
    uint32_t last_interaction_time = esp_log_timestamp();
    int post_game_state = 0;
    uint32_t post_game_start = 0;
    oled_mode_t last_oled_mode = OLED_MODE_NORMAL;

    // Boot-button WiFi-reset state machine
    typedef enum {
        BOOT_IDLE,           // button not held
        BOOT_HOLDING,        // held, counting up to 7 s
        BOOT_CONFIRMING,     // confirm screen showing, waiting for press
    } boot_btn_state_t;
    boot_btn_state_t boot_state = BOOT_IDLE;
    uint32_t boot_hold_start = 0;   // timestamp when hold began
    bool last_boot_btn = false;     // debounced previous level

    while (1) {
        bool btn1 = (gpio_get_level(BTN_1_GPIO) == 0);
        bool btn2 = (gpio_get_level(BTN_2_GPIO) == 0);
        
        g_btn1_state = btn1;
        g_btn2_state = btn2;

        if (g_led_direct_mode) {
            if (btn1 != last_btn1) led_grn_set(btn1);
            if (btn2 != last_btn2) led_red_set(btn2);
        }
        
        oled_set_paddle_input(btn1, btn2);
        
        uint32_t now = esp_log_timestamp();
        if ((btn1 && !last_btn1) || (btn2 && !last_btn2)) {
            buzzer_play_tone(800, 20);
            last_interaction_time = now;
            post_game_state = 0; // cancel mood sequence on manual interaction
        }

        oled_mode_t current_mode = oled_get_mode();
        if (current_mode == OLED_MODE_NORMAL && last_oled_mode != OLED_MODE_NORMAL) {
            post_game_state = 1;
            post_game_start = now;
            oled_set_emotion(EYE_EMOTION_SLEEPY);
            last_interaction_time = now;
        }
        last_oled_mode = current_mode;

        if (current_mode != OLED_MODE_NORMAL && current_mode != OLED_MODE_MENU) {
            if (now - last_interaction_time > 60000) {
                oled_set_mode(OLED_MODE_NORMAL);
            }
        } else if (current_mode == OLED_MODE_MENU) {
            if (now - last_interaction_time > 60000) {
                oled_set_mode(OLED_MODE_NORMAL);
            }
        }

        if (current_mode == OLED_MODE_NORMAL && post_game_state > 0) {
            if (post_game_state == 1 && now - post_game_start > 30000) {
                post_game_state = 2;
                post_game_start = now;
                oled_set_emotion(EYE_EMOTION_MAD);
            } else if (post_game_state == 2 && now - post_game_start > 30000) {
                post_game_state = 0;
                oled_set_emotion(EYE_EMOTION_NORMAL);
            }
        }

        if (btn2) {
            if (!last_btn2) {
                btn2_press_start = now;
            } else if (now - btn2_press_start > 3000 && btn2_press_start > 0) {
                oled_set_mode(OLED_MODE_MENU);
                btn2_press_start = 0;
            }
        } else {
            btn2_press_start = 0;
        }
        
        if (btn1 && btn2) {
            if (!(last_btn1 && last_btn2)) {
                oled_set_mode(OLED_MODE_MENU);
            }
        }
        
        if (oled_get_mode() == OLED_MODE_NORMAL) {
            if (btn1 && !last_btn1 && !btn2) {
                // BTN_1 → Cycle eye emotions
                static int eye_idx = 0;
                static const eye_emotion_t eye_modes[] = {
                    EYE_EMOTION_NORMAL,
                    EYE_EMOTION_MAD,
                    EYE_EMOTION_SAD,
                    EYE_EMOTION_SLEEPY,
                    EYE_EMOTION_SURPRISED
                };
                eye_idx = (eye_idx + 1) % 5;
                oled_set_emotion(eye_modes[eye_idx]);
                buzzer_play_tone(1000,  30);
                ESP_LOGI("BTN", "BTN1: Eye mode idx=%d", eye_idx);
            }
            if (btn2 && !last_btn2 && !btn1) {
                // BTN_2 → Cycle through fun animations
                static int fun_idx = 0;
                static const oled_mode_t fun_modes[] = {
                    OLED_MODE_FIREWORKS,
                    OLED_MODE_MATRIX_RAIN,
                    OLED_MODE_SPACE_INVADER,
                    OLED_MODE_HEARTBEAT,
                    OLED_MODE_NORMAL,
                };
                fun_idx = (fun_idx + 1) % 5;
                oled_set_mode(fun_modes[fun_idx]);
                buzzer_play_tone(1200, 20);
                ESP_LOGI("BTN", "BTN2: Fun mode idx=%d (mode=%d)", fun_idx, fun_modes[fun_idx]);
            }
        }
        
        last_btn1 = btn1;
        last_btn2 = btn2;

        // ── Boot button: servo on short tap, WiFi reset on 7-second hold ────
        bool boot_raw = (gpio_get_level(BTN_BOOT_GPIO) == 0);
        uint32_t boot_now = esp_log_timestamp();

        switch (boot_state) {
            case BOOT_IDLE:
                if (boot_raw && !last_boot_btn) {
                    // Rising edge — start timing the hold
                    boot_hold_start = boot_now;
                    boot_state = BOOT_HOLDING;
                    ESP_LOGI("BTN", "Boot btn held — starting 7s WiFi reset countdown");
                }
                break;

            case BOOT_HOLDING:
                if (!boot_raw) {
                    // Released before 7 s — short tap: do servo action and go idle
                    ESP_LOGI("BTN", "Boot btn short tap -> Servo Hi");
                    servo_quick_action(1, 40, 90);
                    boot_state = BOOT_IDLE;
                } else {
                    uint32_t held_ms = boot_now - boot_hold_start;
                    // Show progress on OLED every ~1 s while holding
                    int held_s = (int)(held_ms / 1000);
                    if (held_s >= 1 && held_s < 7) {
                        // Brief countdown hint (overrides current display for one frame)
                        char hint[32];
                        snprintf(hint, sizeof(hint), "Hold...%ds", 7 - held_s);
                        oled_set_text(hint, 800);
                    }
                    if (held_ms >= 7000) {
                        // 7 seconds reached — enter confirm mode
                        buzzer_play_tone(600, 300);
                        oled_set_mode(OLED_MODE_WIFI_RESET_CONFIRM);
                        boot_state = BOOT_CONFIRMING;
                        ESP_LOGW("BTN", "Boot btn held 7s — showing WiFi reset confirmation");
                    }
                }
                break;

            case BOOT_CONFIRMING:
                // Confirm screen is shown; a fresh button press (rising edge) confirms.
                // If the OLED mode was reset back to NORMAL (timer auto-cancelled),
                // then we silently cancel too.
                if (oled_get_mode() != OLED_MODE_WIFI_RESET_CONFIRM) {
                    // Auto-cancel fired from OLED side
                    boot_state = BOOT_IDLE;
                } else if (boot_raw && !last_boot_btn) {
                    // Rising edge = confirmed — forget all WiFi and restart
                    ESP_LOGW("BTN", "WiFi reset CONFIRMED — erasing credentials and restarting");
                    oled_set_text("Resetting...", 5000);
                    vTaskDelay(pdMS_TO_TICKS(800)); // let OLED show the message
                    wifi_forget_all(); // does not return (calls esp_restart)
                }
                break;
        }
        last_boot_btn = boot_raw;

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
    buzzer_init();
    buzzer_demo_startup();
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
