#include "rf.h"
#include "config.h"
#include "led.h"
#include "servo.h"
#include "RCSwitch.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "mdns.h"

#include "rf_relay_config.h"
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>



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
    bool        relayed;
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

// ── Relay mode state (Photodetector RF -> SpeakerBot Bark) ──────────────────
static volatile bool      s_relay_enabled = false;
static char               s_relay_target_host[64] = RF_RELAY_TARGET;
static char               s_relay_last_event[128] = "Idle";
static int64_t            s_last_relay_bark_time = 0;
static portMUX_TYPE       s_relay_mux   = portMUX_INITIALIZER_UNLOCKED;

void rf_relay_set_config(bool enabled, const char *host) {
    portENTER_CRITICAL(&s_relay_mux);
    s_relay_enabled = enabled;
    if (host && host[0]) {
        strncpy(s_relay_target_host, host, sizeof(s_relay_target_host) - 1);
        s_relay_target_host[sizeof(s_relay_target_host) - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_relay_mux);
    ESP_LOGI(TAG, "RF Relay config set: enabled=%s, target_host=%s",
             enabled ? "true" : "false", s_relay_target_host);
}

void rf_relay_get_config(bool *out_enabled, char *out_host, size_t max_host, char *out_last_event, size_t max_event) {
    portENTER_CRITICAL(&s_relay_mux);
    if (out_enabled) *out_enabled = s_relay_enabled;
    if (out_host && max_host > 0) {
        strncpy(out_host, s_relay_target_host, max_host - 1);
        out_host[max_host - 1] = '\0';
    }
    if (out_last_event && max_event > 0) {
        strncpy(out_last_event, s_relay_last_event, max_event - 1);
        out_last_event[max_event - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_relay_mux);
}

bool rf_relay_is_enabled(void) {
    return s_relay_enabled;
}

static void trigger_speakerbot_bark_task(void *pvParameters) {
    char target[64] = {0};
    portENTER_CRITICAL(&s_relay_mux);
    snprintf(target, sizeof(target), "%s", s_relay_target_host);
    portEXIT_CRITICAL(&s_relay_mux);

    const char *h = target;
    if (strncmp(h, "http://", 7) == 0) h += 7;
    else if (strncmp(h, "https://", 8) == 0) h += 8;

    char host_or_ip[64] = {0};
    char path[64] = "/bark";

    const char *slash = strchr(h, '/');
    if (slash != NULL) {
        size_t len = slash - h;
        if (len >= sizeof(host_or_ip)) len = sizeof(host_or_ip) - 1;
        strncpy(host_or_ip, h, len);
        host_or_ip[len] = '\0';
        strncpy(path, slash, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        strncpy(host_or_ip, h, sizeof(host_or_ip) - 1);
        host_or_ip[sizeof(host_or_ip) - 1] = '\0';
    }

    ESP_LOGI(TAG, "RF Relay triggering: host='%s', path='%s'", host_or_ip, path);

    // 1. Try mDNS resolution if it ends with .local or has no dots
    char ip_str[32] = {0};
    if (strstr(host_or_ip, ".local") != NULL || strchr(host_or_ip, '.') == NULL) {
        char clean_name[64] = {0};
        strncpy(clean_name, host_or_ip, sizeof(clean_name) - 1);
        char *loc = strstr(clean_name, ".local");
        if (loc) *loc = '\0';

        esp_ip4_addr_t addr;
        addr.addr = 0;
        ESP_LOGI(TAG, "mDNS querying A record for '%s'...", clean_name);
        esp_err_t err = mdns_query_a(clean_name, 2000, &addr);
        if (err != ESP_OK || addr.addr == 0) {
            err = mdns_query_a(host_or_ip, 2000, &addr);
        }
        if (err == ESP_OK && addr.addr != 0) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&addr));
            ESP_LOGI(TAG, "mDNS successfully resolved '%s' -> %s", clean_name, ip_str);
        } else {
            ESP_LOGW(TAG, "mDNS query failed for '%s' (err=%d)", clean_name, err);
        }
    }

    const char *connect_host = (ip_str[0] != '\0') ? ip_str : host_or_ip;

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    int lookup_err = getaddrinfo(connect_host, "80", &hints, &res);
    if (lookup_err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS/Socket lookup failed for '%s': err=%d", connect_host, lookup_err);
        portENTER_CRITICAL(&s_relay_mux);
        snprintf(s_relay_last_event, sizeof(s_relay_last_event), "\xe2\x9d\x8c Lookup failed: %s", connect_host);
        portEXIT_CRITICAL(&s_relay_mux);
        vTaskDelete(NULL);
        return;
    }

    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "Failed to allocate socket");
        freeaddrinfo(res);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Socket connect to %s:80 failed!", connect_host);
        portENTER_CRITICAL(&s_relay_mux);
        snprintf(s_relay_last_event, sizeof(s_relay_last_event), "\xe2\x9d\x8c Connect failed: %s:80", connect_host);
        portEXIT_CRITICAL(&s_relay_mux);
        close(s);
        freeaddrinfo(res);
        vTaskDelete(NULL);
        return;
    }
    freeaddrinfo(res);

    char http_req[256];
    int req_len = snprintf(http_req, sizeof(http_req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: RFBot-Relay\r\n"
        "Connection: close\r\n\r\n",
        path, host_or_ip);

    if (send(s, http_req, req_len, 0) < 0) {
        ESP_LOGE(TAG, "Socket send failed!");
        portENTER_CRITICAL(&s_relay_mux);
        snprintf(s_relay_last_event, sizeof(s_relay_last_event), "\xe2\x9d\x8c Send failed to %s", connect_host);
        portEXIT_CRITICAL(&s_relay_mux);
    } else {
        ESP_LOGI(TAG, "HTTP GET %s sent to %s successfully!", path, connect_host);
        portENTER_CRITICAL(&s_relay_mux);
        snprintf(s_relay_last_event, sizeof(s_relay_last_event), "\xe2\x9c\x93 Sent GET %s -> %s", path, connect_host);
        portEXIT_CRITICAL(&s_relay_mux);
    }
    close(s);
    vTaskDelete(NULL);
}

void rf_relay_trigger(void) {
    ESP_LOGI(TAG, "Manual RF Relay test triggered!");
    xTaskCreate(trigger_speakerbot_bark_task, "spk_bark", 8192, NULL, 5, NULL);
}

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
        if (local[i].relayed) {
            cJSON_AddBoolToObject(obj, "relayed", true);
        }
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

            bool is_relayed = (s_relay_enabled && (code == RF_RELAY_CODE || code == 123456 || code == 0x123456));

            // ── Listen mode — buffer every packet for the WebUI ──────────────
            if (s_listen_mode) {
                portENTER_CRITICAL(&s_listen_mux);
                s_listen_buf[s_listen_head].code     = code;
                s_listen_buf[s_listen_head].bits     = bits;
                s_listen_buf[s_listen_head].protocol = proto;
                s_listen_buf[s_listen_head].pulse    = pulse;
                s_listen_buf[s_listen_head].relayed  = is_relayed;
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

            // ── Photodetector RF Relay (runs whenever relay mode is enabled) ──
            if (s_relay_enabled && (code == RF_RELAY_CODE || code == 123456 || code == 0x123456)) {
                int64_t now = esp_timer_get_time() / 1000;
                if (now - s_last_relay_bark_time >= 1500) {
                    s_last_relay_bark_time = now;
                    ESP_LOGI(TAG, "RF Relay: photodetector code %lu detected! Relaying request to %s...",
                             (unsigned long)code, s_relay_target_host);
                    xTaskCreate(trigger_speakerbot_bark_task, "spk_bark", 8192, NULL, 5, NULL);
                } else {
                    ESP_LOGD(TAG, "RF Relay: bark request rate-limited");
                }
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
                        if (!s_relay_enabled || (code != RF_RELAY_CODE && code != 123456 && code != 0x123456)) {
                            ESP_LOGW(TAG, "Unknown code, ignoring");
                        }
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
