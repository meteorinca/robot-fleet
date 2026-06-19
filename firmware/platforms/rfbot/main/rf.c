#include "rf.h"
#include "config.h"
#include "led.h"
#include "servo.h"
#include "RCSwitch.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "RF";

volatile uint32_t rf_isr_edge_count = 0;

// ── Separate TX and RX structs to avoid conflicts ───────────────────────────
static RCSWITCH_t s_rc_tx;
static RCSWITCH_t s_rc_rx;

// ── Shared packet type ───────────────────────────────────────────────────────
typedef struct {
    uint32_t    code;
    unsigned    bits;
    unsigned    protocol;
    unsigned    pulse;
} rf_packet_t;

// ── Learn mode state ─────────────────────────────────────────────────────────
#define RF_LEARN_BUF_SIZE 8

static volatile bool      s_learn_mode  = false;
static rf_packet_t        s_learn_buf[RF_LEARN_BUF_SIZE];
static volatile int       s_learn_count = 0;
static portMUX_TYPE       s_learn_mux   = portMUX_INITIALIZER_UNLOCKED;

// ── Listen mode state ─────────────────────────────────────────────────────────
// Accumulates all received packets for the WebUI to poll via /rf/poll.
// Implements a circular ring buffer — oldest entry is overwritten when full.
#define RF_LISTEN_BUF_SIZE 32

static volatile bool      s_listen_mode  = false;
static rf_packet_t        s_listen_buf[RF_LISTEN_BUF_SIZE];
static volatile int       s_listen_head  = 0;   // write index (next slot to fill)
static volatile int       s_listen_count = 0;   // total packets waiting to be read
static portMUX_TYPE       s_listen_mux   = portMUX_INITIALIZER_UNLOCKED;

void rf_module_init(void) {
    // TX
    initSwitch(&s_rc_tx);
    enableTransmit(&s_rc_tx, RF_TX_GPIO);

    // RX — will be started in rf_start_receiver()
    initSwitch(&s_rc_rx);
}

void rf_send_code(uint32_t code, unsigned int bit_length) {
    sendCode(&s_rc_tx, code, bit_length);
}

void rf_send_full(uint32_t code, unsigned int bit_length, int protocol, int pulse_length) {
    /* Save current protocol so we can restore it cleanly after the send. */
    Protocol saved = s_rc_tx.protocol;

    if (protocol > 0) setProtocol(&s_rc_tx, protocol);
    if (pulse_length > 0) setPulseLength(&s_rc_tx, pulse_length);

    ESP_LOGI(TAG, "rf_send_full: code=0x%lX bits=%u proto=%d pulse=%d",
             (unsigned long)code, bit_length, protocol, pulse_length);
    sendCode(&s_rc_tx, code, bit_length);

    /* Restore previous protocol (including its pulseLength). */
    s_rc_tx.protocol = saved;
}

// ── Learn mode API ───────────────────────────────────────────────────────────
void rf_learn_start(void) {
    portENTER_CRITICAL(&s_learn_mux);
    s_learn_count = 0;
    s_learn_mode = true;
    portEXIT_CRITICAL(&s_learn_mux);
    ESP_LOGI(TAG, "Learn mode ON");
}

void rf_learn_stop(void) {
    portENTER_CRITICAL(&s_learn_mux);
    s_learn_mode = false;
    portEXIT_CRITICAL(&s_learn_mux);
    ESP_LOGI(TAG, "Learn mode OFF");
}

bool rf_learn_active(void) {
    return s_learn_mode;
}

cJSON *rf_learn_get_codes(void) {
    cJSON *arr = cJSON_CreateArray();

    portENTER_CRITICAL(&s_learn_mux);
    int n = s_learn_count;
    rf_packet_t local[RF_LEARN_BUF_SIZE];
    if (n > 0) {
        memcpy(local, s_learn_buf, n * sizeof(rf_packet_t));
        s_learn_count = 0;
    }
    portEXIT_CRITICAL(&s_learn_mux);

    for (int i = 0; i < n; i++) {
        cJSON *obj = cJSON_CreateObject();
        char hex[12];
        snprintf(hex, sizeof(hex), "%lX", (unsigned long)local[i].code);
        cJSON_AddStringToObject(obj, "code", hex);
        cJSON_AddNumberToObject(obj, "bits", local[i].bits);
        cJSON_AddNumberToObject(obj, "proto", local[i].protocol);
        cJSON_AddNumberToObject(obj, "pulse", local[i].pulse);
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

// ── Listen mode API ──────────────────────────────────────────────────────────

void rf_listen_start(void) {
    portENTER_CRITICAL(&s_listen_mux);
    s_listen_head  = 0;
    s_listen_count = 0;
    s_listen_mode  = true;
    portEXIT_CRITICAL(&s_listen_mux);
    ESP_LOGI(TAG, "Listen mode ON");
}

void rf_listen_stop(void) {
    portENTER_CRITICAL(&s_listen_mux);
    s_listen_mode = false;
    portEXIT_CRITICAL(&s_listen_mux);
    ESP_LOGI(TAG, "Listen mode OFF");
}

bool rf_listen_active(void) {
    return s_listen_mode;
}

// Returns a cJSON array of all buffered packets since the last call, then
// clears the buffer.  Each element: {"code":"1A2B","bits":24,"proto":1,"pulse":350}
// Caller must cJSON_Delete() the returned value.
cJSON *rf_listen_get_packets(void) {
    cJSON *arr = cJSON_CreateArray();

    portENTER_CRITICAL(&s_listen_mux);
    int n = s_listen_count;
    if (n > RF_LISTEN_BUF_SIZE) n = RF_LISTEN_BUF_SIZE;

    rf_packet_t local[RF_LISTEN_BUF_SIZE];
    if (n > 0) {
        // Reconstruct ordered slice from circular buffer.
        // s_listen_head points to the next write slot; read backwards.
        int start = (s_listen_head - n + RF_LISTEN_BUF_SIZE) % RF_LISTEN_BUF_SIZE;
        for (int i = 0; i < n; i++) {
            local[i] = s_listen_buf[(start + i) % RF_LISTEN_BUF_SIZE];
        }
        // Clear after read
        s_listen_head  = 0;
        s_listen_count = 0;
    }
    portEXIT_CRITICAL(&s_listen_mux);

    for (int i = 0; i < n; i++) {
        cJSON *obj = cJSON_CreateObject();
        char hex[12];
        snprintf(hex, sizeof(hex), "%lX", (unsigned long)local[i].code);
        cJSON_AddStringToObject(obj, "code", hex);
        cJSON_AddNumberToObject(obj, "bits", local[i].bits);
        cJSON_AddNumberToObject(obj, "proto", local[i].protocol);
        cJSON_AddNumberToObject(obj, "pulse", local[i].pulse);
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

// ── Confirmation TX (existing logic) ────────────────────────────────────────
static void send_confirmation(uint32_t original_code) {
    uint32_t confirm = original_code ^ 0xAAAAAA;
    sendCode(&s_rc_tx, confirm, 24);
    ESP_LOGI(TAG, "Sent confirmation 0x%06lX", (unsigned long)confirm);
    vTaskDelay(pdMS_TO_TICKS(500));
}

// ── Receiver task ────────────────────────────────────────────────────────────
static void rf_receiver_task(void *pvParameters) {
    enableReceive(&s_rc_rx, RF_RX_GPIO);
    while (1) {
        if (available(&s_rc_rx)) {
            uint32_t   code  = getReceivedValue(&s_rc_rx);
            unsigned   bits  = getReceivedBitlength(&s_rc_rx);
            unsigned   proto = getReceivedProtocol(&s_rc_rx);
            unsigned   pulse = getReceivedDelay(&s_rc_rx);
            resetAvailable(&s_rc_rx);

            ESP_LOGI(TAG, "Received 0x%lX bits=%u proto=%u pulse=%u",
                     (unsigned long)code, bits, proto, pulse);

            // ── Listen mode — buffer every packet for the WebUI ──────────────
            if (s_listen_mode) {
                portENTER_CRITICAL(&s_listen_mux);
                s_listen_buf[s_listen_head].code     = code;
                s_listen_buf[s_listen_head].bits     = bits;
                s_listen_buf[s_listen_head].protocol = proto;
                s_listen_buf[s_listen_head].pulse    = pulse;
                s_listen_head = (s_listen_head + 1) % RF_LISTEN_BUF_SIZE;
                if (s_listen_count < RF_LISTEN_BUF_SIZE) s_listen_count++;
                portEXIT_CRITICAL(&s_listen_mux);
            }

            // ── Learn mode — dedicated capture buffer ────────────────────────
            if (s_learn_mode) {
                portENTER_CRITICAL(&s_learn_mux);
                if (s_learn_count < RF_LEARN_BUF_SIZE) {
                    s_learn_buf[s_learn_count].code     = code;
                    s_learn_buf[s_learn_count].bits     = bits;
                    s_learn_buf[s_learn_count].protocol = proto;
                    s_learn_buf[s_learn_count].pulse    = pulse;
                    s_learn_count++;
                }
                portEXIT_CRITICAL(&s_learn_mux);
            }

            // ── Normal action dispatch (only when not in learn/listen mode) ──
            if (!s_learn_mode && !s_listen_mode) {
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
                        ESP_LOGW(TAG, "Unknown code, ignoring");
                        break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void rf_start_receiver(void) {
    xTaskCreate(rf_receiver_task, "rf_rx", 4096, NULL, 5, NULL);
}
