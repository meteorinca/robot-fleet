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
#include "esp_random.h"


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

    // Boot button state is now handled inline below

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

        // ── Boot button: cycle eyes on short tap, WiFi reset on 7-second hold ────
        if (gpio_get_level(BTN_BOOT_GPIO) == 0) {
            int hold_time = 0;
            bool reset_triggered = false;
            while(gpio_get_level(BTN_BOOT_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                hold_time += 100;
                if (hold_time >= 4000 && hold_time < 7000) {
                    int countdown = 7 - (hold_time / 1000);
                    char msg[32];
                    snprintf(msg, sizeof(msg), "Hold to\nReset: %d", countdown);
                    oled_set_text(msg, 200);
                }
                if (hold_time >= 7000) {
                    reset_triggered = true;
                    break;
                }
            }
            
            if (reset_triggered) {
                while(gpio_get_level(BTN_BOOT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                oled_set_text("Triple click\nto confirm", 5000);
                
                int click_count = 0;
                int timeout = 5000;
                while(timeout > 0) {
                    if (gpio_get_level(BTN_BOOT_GPIO) == 0) {
                        click_count++;
                        while(gpio_get_level(BTN_BOOT_GPIO) == 0) {
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
                    oled_set_text("RESETTING\nWIFI...", 5000);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    wifi_forget_all(); // Does not return
                } else {
                    oled_set_text("", 0); // clear
                    oled_set_text("Cancelled", 2000);
                }
            } else {
                oled_set_text("", 0); // clear
                if (hold_time < 4000) {
                    // Short tap: Cycle eyes and play random-ish sound
                    static int eye_idx2 = 0;
                    static const eye_emotion_t eye_modes2[] = {
                        EYE_EMOTION_NORMAL,
                        EYE_EMOTION_MAD,
                        EYE_EMOTION_SAD,
                        EYE_EMOTION_SLEEPY,
                        EYE_EMOTION_SURPRISED
                    };
                    eye_idx2 = (eye_idx2 + 1) % 5;
                    oled_set_emotion(eye_modes2[eye_idx2]);
                    buzzer_play_tone(800 + (esp_random() % 600), 40 + (esp_random() % 40));
                } else {
                    oled_set_text("Cancelled", 2000);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30)); // fast poll for games
    }
}


static void boot_msg_task(void *arg) {
    led_action_set(true); // LED on early!
    buzzer_demo_mario(); // Mario sound early!
    oled_set_text("\'Ello\nHuman!", 6000);
    vTaskDelay(pdMS_TO_TICKS(2000)); // Give user a chance to read the boot message
    oled_set_text(" Mr MoJ \nsends his \nREGARDS", 6000);
    vTaskDelay(pdMS_TO_TICKS(3000)); // Give user a chance to read the boot message
    led_action_set(false);
    vTaskDelete(NULL);
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
    // buzzer_demo_startup(); // Replaced by Mario in boot sequence
    servo_init();
    servo_worker_start();
    ultrasonic_init();
    oled_init();

    // Start non-blocking boot message sequence
    xTaskCreate(boot_msg_task, "boot_msg", 2048, NULL, 5, NULL);

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
