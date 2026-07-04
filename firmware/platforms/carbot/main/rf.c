#include "rf.h"
#include "config.h"
#include "led.h"
#include "servo.h"
#include "rcswitch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static RCSWITCH_t s_rc;

void rf_module_init(void) {
    initSwitch(&s_rc);
    enableTransmit(&s_rc, RF_TX_GPIO);
}

void rf_send_code(uint32_t code, unsigned int bit_length) {
    sendCode(&s_rc, code, bit_length);
}

static void send_confirmation(uint32_t original_code) {
    uint32_t confirm = original_code ^ 0xAAAAAA;
    sendCode(&s_rc, confirm, 24);
    ESP_LOGI("RF", "Sent confirmation 0x%06lX", (unsigned long)confirm);
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void rf_receiver_task(void *pvParameters) {
    enableReceive(&s_rc, RF_RX_GPIO);
    while (1) {
        if (available(&s_rc)) {
            uint32_t code = getReceivedValue(&s_rc);
            resetAvailable(&s_rc);
            ESP_LOGI("RF", "Received 0x%06lX", (unsigned long)code);

            switch (code) {
                case RF_CODE_TOGGLE_LED:
                    led_action_toggle();
                    send_confirmation(code);
                    break;
                case RF_CODE_SERVO1:
                    servo_action_set(1, 0);
                    send_confirmation(code);
                    break;
                default:
                    ESP_LOGW("RF", "Unknown code, ignoring");
                    break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void rf_start_receiver(void) {
    xTaskCreate(rf_receiver_task, "rf_rx", 4096, NULL, 5, NULL);
}
