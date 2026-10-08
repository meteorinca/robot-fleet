#include "wifi_mgr.h"
#include "webserver.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#define TAG "WIFI"

#ifndef MAX_NVS_NETWORKS
#define MAX_NVS_NETWORKS 5
#endif

#ifndef AP_FALLBACK_TIMEOUT_MS
#define AP_FALLBACK_TIMEOUT_MS  10000   // default 10s per SSID before fallback
#endif

#define NVS_WIFI_NAMESPACE  "wifi_creds"
#define NVS_KEY_COUNT       "count"

#define ENABLE_CAPTIVE_PORTAL 1

// ── State ────────────────────────────────────────────────────────────────────
static EventGroupHandle_t s_wifi_events;
static TimerHandle_t      s_ap_timer      = NULL;
static bool               s_ap_active     = false;
static int                s_retry_count   = 0;
static char               s_ip_addr[32] = {0};

// ── Combined network list ────────────────────────────────────────────────────
#define MAX_TOTAL_NETWORKS  (MAX_NVS_NETWORKS + 5)

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

static wifi_cred_t s_networks[MAX_TOTAL_NETWORKS];
static int         s_network_count = 0;
static int         s_network_idx   = 0;

// ══════════════════════════════════════════════════════════════════════════════
//  NVS credential storage
// ══════════════════════════════════════════════════════════════════════════════

static void nvs_load_credentials(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    int32_t count = 0;
    nvs_get_i32(h, NVS_KEY_COUNT, &count);

    for (int i = 0; i < count && i < MAX_NVS_NETWORKS && s_network_count < MAX_TOTAL_NETWORKS; i++) {
        char key_s[32], key_p[32];
        snprintf(key_s, sizeof(key_s), "ssid_%d", i);
        snprintf(key_p, sizeof(key_p), "pass_%d", i);

        char temp_s[sizeof(s_networks[0].ssid)] = {0};
        char temp_p[sizeof(s_networks[0].pass)] = {0};
        size_t slen = sizeof(temp_s);
        size_t plen = sizeof(temp_p);

        if (nvs_get_str(h, key_s, temp_s, &slen) == ESP_OK &&
            nvs_get_str(h, key_p, temp_p, &plen) == ESP_OK &&
            temp_s[0] != '\0') {
            bool duplicate = false;
            for (int k = 0; k < s_network_count; k++) {
                if (strcmp(s_networks[k].ssid, temp_s) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                strncpy(s_networks[s_network_count].ssid, temp_s, sizeof(s_networks[0].ssid) - 1);
                strncpy(s_networks[s_network_count].pass, temp_p, sizeof(s_networks[0].pass) - 1);
                ESP_LOGI(TAG, "NVS WiFi %d: %s", s_network_count + 1, temp_s);
                s_network_count++;
            }
        }
    }
    nvs_close(h);
}

esp_err_t wifi_save_credential(const char *ssid, const char *pass) {
    if (!ssid || !pass) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    // Read current count
    int32_t count = 0;
    nvs_get_i32(h, NVS_KEY_COUNT, &count);

    // Check if this SSID already exists — if so, update in-place
    for (int i = 0; i < count; i++) {
        char key_s[32], existing_ssid[33] = {0};
        snprintf(key_s, sizeof(key_s), "ssid_%d", i);
        size_t slen = sizeof(existing_ssid);
        if (nvs_get_str(h, key_s, existing_ssid, &slen) == ESP_OK &&
            strcmp(existing_ssid, ssid) == 0) {
            // Update password only
            char key_p[32];
            snprintf(key_p, sizeof(key_p), "pass_%d", i);
            nvs_set_str(h, key_p, pass);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "Updated WiFi credential: %s (slot %d)", ssid, i);
            return ESP_OK;
        }
    }

    // New entry — shift everything down to make this the first (highest priority)
    if (count >= MAX_NVS_NETWORKS) {
        count = MAX_NVS_NETWORKS - 1;  // drop the oldest
    }
    // Shift existing entries down by one
    for (int i = count - 1; i >= 0; i--) {
        char src_s[32], src_p[32], dst_s[32], dst_p[32];
        snprintf(src_s, sizeof(src_s), "ssid_%d", i);
        snprintf(src_p, sizeof(src_p), "pass_%d", i);
        snprintf(dst_s, sizeof(dst_s), "ssid_%d", i + 1);
        snprintf(dst_p, sizeof(dst_p), "pass_%d", i + 1);

        char tmp_ssid[33] = {0}, tmp_pass[65] = {0};
        size_t sl = sizeof(tmp_ssid), pl = sizeof(tmp_pass);
        nvs_get_str(h, src_s, tmp_ssid, &sl);
        nvs_get_str(h, src_p, tmp_pass, &pl);
        nvs_set_str(h, dst_s, tmp_ssid);
        nvs_set_str(h, dst_p, tmp_pass);
    }

    // Write new entry at index 0 (highest priority)
    nvs_set_str(h, "ssid_0", ssid);
    nvs_set_str(h, "pass_0", pass);
    nvs_set_i32(h, NVS_KEY_COUNT, count + 1);
    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Saved new WiFi credential: %s (total: %ld)", ssid, (long)(count + 1));
    return err;
}

int wifi_nvs_credential_count(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    int32_t count = 0;
    nvs_get_i32(h, NVS_KEY_COUNT, &count);
    nvs_close(h);
    return (int)count;
}

bool wifi_nvs_credential_get(int index, char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    int32_t count = 0;
    nvs_get_i32(h, NVS_KEY_COUNT, &count);
    if (index < 0 || index >= count) { nvs_close(h); return false; }

    char key_s[32], key_p[32];
    snprintf(key_s, sizeof(key_s), "ssid_%d", index);
    snprintf(key_p, sizeof(key_p), "pass_%d", index);

    bool ok = (nvs_get_str(h, key_s, ssid, &ssid_len) == ESP_OK &&
               nvs_get_str(h, key_p, pass, &pass_len) == ESP_OK);
    nvs_close(h);
    return ok;
}

esp_err_t wifi_nvs_credential_delete(int index) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    int32_t count = 0;
    nvs_get_i32(h, NVS_KEY_COUNT, &count);
    if (index < 0 || index >= count) { nvs_close(h); return ESP_ERR_INVALID_ARG; }

    // Shift entries after the deleted one up
    for (int i = index; i < count - 1; i++) {
        char src_s[32], src_p[32], dst_s[32], dst_p[32];
        snprintf(src_s, sizeof(src_s), "ssid_%d", i + 1);
        snprintf(src_p, sizeof(src_p), "pass_%d", i + 1);
        snprintf(dst_s, sizeof(dst_s), "ssid_%d", i);
        snprintf(dst_p, sizeof(dst_p), "pass_%d", i);

        char tmp_ssid[33] = {0}, tmp_pass[65] = {0};
        size_t sl = sizeof(tmp_ssid), pl = sizeof(tmp_pass);
        nvs_get_str(h, src_s, tmp_ssid, &sl);
        nvs_get_str(h, src_p, tmp_pass, &pl);
        nvs_set_str(h, dst_s, tmp_ssid);
        nvs_set_str(h, dst_p, tmp_pass);
    }

    // Erase the last entry
    char last_s[32], last_p[32];
    snprintf(last_s, sizeof(last_s), "ssid_%d", (int)(count - 1));
    snprintf(last_p, sizeof(last_p), "pass_%d", (int)(count - 1));
    nvs_erase_key(h, last_s);
    nvs_erase_key(h, last_p);
    nvs_set_i32(h, NVS_KEY_COUNT, count - 1);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void wifi_forget_all(void) {
    ESP_LOGW(TAG, "wifi_forget_all: erasing all WiFi credentials and restarting...");
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    // A hard restart is the cleanest way to re-enter AP mode from a clean slate.
    esp_restart();
}

// ══════════════════════════════════════════════════════════════════════════════
//  Captive portal DNS — responds to ALL DNS queries with 192.168.4.1
//  This makes phones auto-open the portal when they connect to the SoftAP.
// ══════════════════════════════════════════════════════════════════════════════
#if ENABLE_CAPTIVE_PORTAL

static void dns_server_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive portal DNS running on :53");
    uint8_t buf[512];

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;  // too short for DNS header

        uint8_t resp[512];
        memcpy(resp, buf, len);  // start with the query

        // Flags: response (0x80), recursion desired (0x01), authoritative (0x04)
        resp[2] = 0x81;
        resp[3] = 0x80;
        // Answer count = 1
        resp[6] = 0x00;
        resp[7] = 0x01;

        // Append answer: pointer to name in question (0xC00C), type A, class IN, TTL 60, IP
        int rlen = len;
        resp[rlen++] = 0xC0;  // name pointer
        resp[rlen++] = 0x0C;  // offset to question name
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  // type A
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  // class IN
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3C;  // TTL = 60s
        resp[rlen++] = 0x00; resp[rlen++] = 0x04;  // data length = 4
        resp[rlen++] = 192;  // 192.168.4.1
        resp[rlen++] = 168;
        resp[rlen++] = 4;
        resp[rlen++] = 1;

        sendto(sock, resp, rlen, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }
}

static void start_captive_dns(void) {
    xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 3, NULL);
}
#endif // ENABLE_CAPTIVE_PORTAL

// ══════════════════════════════════════════════════════════════════════════════
//  SoftAP setup
//  AP SSID: "RFBot-XX"  (XX = device number from board_config.h)
//  AP IP:   192.168.4.1  (ESP32 default)
// ══════════════════════════════════════════════════════════════════════════════

bool wifi_is_ap_mode(void) {
    return s_ap_active;
}

wifi_state_t wifi_mgr_get_state(void) {
    if (s_ap_active) return WIFI_STATE_AP_MODE;
    if (s_wifi_events && (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) return WIFI_STATE_CONNECTED;
    return WIFI_STATE_CONNECTING;
}

const char* wifi_mgr_get_ip(void) {
    return s_ip_addr;
}

static void start_softap(void) {
    if (s_ap_active) return;
    s_ap_active = true;

    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = 1,
            .authmode       = WIFI_AUTH_OPEN,   // no password
            .max_connection = 4,
            .beacon_interval = 100,
        },
    };
    
    // Build SSID "RFBot-<device_num>"
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid),
             "RFBot-%d", DEVICE_NUMBER);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);

    esp_wifi_disconnect();
    // Switch to APSTA mode so SoftAP is active AND STA interface can scan for networks
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    esp_err_t s_err = esp_wifi_start();
    if (s_err != ESP_OK && s_err != ESP_ERR_WIFI_STATE) {
        ESP_ERROR_CHECK(s_err);
    }
    esp_wifi_set_max_tx_power(78);

    ESP_LOGI(TAG, "SoftAP started — SSID: '%s'  IP: 192.168.4.1 (APSTA mode, Channel %d)",
             (char *)ap_cfg.ap.ssid, ap_cfg.ap.channel);

#if ENABLE_CAPTIVE_PORTAL
    start_captive_dns();
#endif

    // Start the web server on the AP interface too
    webserver_start();
}

static void advance_to_next_network(void) {
    if (s_ap_active) return;

    s_retry_count = 0;
    s_network_idx++;
    if (s_network_idx < s_network_count) {
        ESP_LOGW(TAG, "Trying next network %d/%d: %s",
                 s_network_idx + 1, s_network_count, s_networks[s_network_idx].ssid);

        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = (s_networks[s_network_idx].pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK,
                .pmf_cfg = {
                    .capable = true,
                    .required = false,
                },
                .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            },
        };
        strncpy((char*)wifi_config.sta.ssid,     s_networks[s_network_idx].ssid,
                sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, s_networks[s_network_idx].pass,
                sizeof(wifi_config.sta.password));
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
        if (s_ap_timer) xTimerReset(s_ap_timer, 0);
    } else {
        ESP_LOGW(TAG, "All %d configured networks failed. Starting SoftAP hotspot mode...", s_network_count);
        start_softap();
    }
}

static void ap_fallback_cb(TimerHandle_t xTimer) {
    advance_to_next_network();
}

// ── ESP-NETIF style event handler ──
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *event_data) {
    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGW(TAG, "==== HARDWARE RF DIAGNOSTIC: Scanning 2.4 GHz spectrum... ====");
        int8_t max_pwr = 0;
        esp_wifi_get_max_tx_power(&max_pwr);
        ESP_LOGI(TAG, "Max TX Power: %d (%.2f dBm)", max_pwr, max_pwr * 0.25f);

        wifi_scan_config_t scan_cfg = {
            .show_hidden = true,
        };
        esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
        if (scan_err != ESP_OK) {
            ESP_LOGE(TAG, "RF SCAN FAILED! esp_wifi_scan_start error: %s", esp_err_to_name(scan_err));
        } else {
            uint16_t ap_num = 0;
            esp_wifi_scan_get_ap_num(&ap_num);
            ESP_LOGW(TAG, "RF SCAN RESULT: Detected %d access point(s)", ap_num);
            if (ap_num > 0) {
                uint16_t max_records = (ap_num > 20) ? 20 : ap_num;
                wifi_ap_record_t *records = malloc(max_records * sizeof(wifi_ap_record_t));
                if (records) {
                    esp_wifi_scan_get_ap_records(&max_records, records);
                    for (int i = 0; i < max_records; i++) {
                        ESP_LOGI(TAG, "  [%02d] SSID: '%-20s' | RSSI: %4d dBm | Ch: %2d | Auth: %d",
                                 i + 1, (char*)records[i].ssid, records[i].rssi,
                                 records[i].primary, records[i].authmode);
                    }
                    free(records);
                }
            } else {
                ESP_LOGE(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                ESP_LOGE(TAG, ">>> HARDWARE FAILURE: 0 NETWORKS HEARD ON 2.4 GHz! <<<");
                ESP_LOGE(TAG, ">>> The ESP32 RF radio / antenna circuit is defective! <<<");
                ESP_LOGE(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
            }
        }
        ESP_LOGW(TAG, "===============================================================");

        if (s_network_count == 0) {
            ESP_LOGW(TAG, "No networks configured — starting SoftAP immediately");
            advance_to_next_network();
        } else {
            esp_wifi_connect();
            if (s_ap_timer) xTimerReset(s_ap_timer, 0);
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (!s_ap_active && s_network_idx < s_network_count) {
            s_retry_count++;
            ESP_LOGW(TAG, "Disconnected (reason=%d, attempt %d/3) on '%s'",
                     disc->reason, s_retry_count, s_networks[s_network_idx].ssid);
            if (s_retry_count >= 3) {
                ESP_LOGW(TAG, "Failed 3 attempts on '%s' — advancing to next network",
                         s_networks[s_network_idx].ssid);
                if (s_ap_timer) xTimerStop(s_ap_timer, 0);
                advance_to_next_network();
            } else {
                esp_wifi_connect();
            }
        }
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = event_data;
        ESP_LOGI(TAG, "AP client connected: " MACSTR, MAC2STR(ev->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = event_data;
        ESP_LOGI(TAG, "AP client disconnected: " MACSTR, MAC2STR(ev->mac));
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *event_data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_addr);
        if (s_ap_timer) xTimerStop(s_ap_timer, 0);
        
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        webserver_start();
    }
}

// ── Build the combined network list ──────────────────────────────────────────
static void build_network_list(void) {
    s_network_count = 0;
    s_network_idx   = 0;

    // 1) NVS-stored credentials (saved by user from WebUI)
    nvs_load_credentials();

    // 2) Hardcoded credentials in priority order from secrets.h
    const char *hardcoded_ssids[] = {
        WIFI_SSID_1,
#ifdef WIFI_SSID_2
        WIFI_SSID_2,
#endif
#ifdef WIFI_SSID_3
        WIFI_SSID_3,
#endif
#ifdef WIFI_SSID_4
        WIFI_SSID_4,
#endif
#ifdef WIFI_SSID_5
        WIFI_SSID_5,
#endif
    };
    const char *hardcoded_passes[] = {
        WIFI_PASS_1,
#ifdef WIFI_PASS_2
        WIFI_PASS_2,
#endif
#ifdef WIFI_PASS_3
        WIFI_PASS_3,
#endif
#ifdef WIFI_PASS_4
        WIFI_PASS_4,
#endif
#ifdef WIFI_PASS_5
        WIFI_PASS_5,
#endif
    };

    int num_hardcoded = sizeof(hardcoded_ssids) / sizeof(hardcoded_ssids[0]);
    for (int h = 0; h < num_hardcoded && s_network_count < MAX_TOTAL_NETWORKS; h++) {
        if (!hardcoded_ssids[h] || hardcoded_ssids[h][0] == '\0') continue;
        bool duplicate = false;
        for (int k = 0; k < s_network_count; k++) {
            if (strcmp(s_networks[k].ssid, hardcoded_ssids[h]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            strncpy(s_networks[s_network_count].ssid, hardcoded_ssids[h],
                    sizeof(s_networks[0].ssid) - 1);
            strncpy(s_networks[s_network_count].pass, hardcoded_passes[h],
                    sizeof(s_networks[0].pass) - 1);
            ESP_LOGI(TAG, "Configured WiFi %d: %s", s_network_count + 1, hardcoded_ssids[h]);
            s_network_count++;
        }
    }

    ESP_LOGI(TAG, "Total networks to try in order: %d", s_network_count);
}

EventGroupHandle_t wifi_init(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap(); // Create AP netif early for mDNS
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, &h_ip));

    build_network_list();

    // Create the AP-fallback timer BEFORE starting Wi-Fi
    s_ap_timer = xTimerCreate("ap_fallback",
                              pdMS_TO_TICKS(AP_FALLBACK_TIMEOUT_MS),
                              pdFALSE, NULL, ap_fallback_cb);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    if (s_network_count == 0) {
        ESP_LOGW(TAG, "No networks configured in secrets.h — starting SoftAP mode directly!");
        start_softap();
    } else {
        strncpy((char*)wifi_config.sta.ssid, s_networks[0].ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, s_networks[0].pass, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = (s_networks[0].pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // mDNS (works regardless of STA/AP mode)
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE));
    mdns_service_add(MDNS_INSTANCE, "_http", "_tcp", WEB_SERVER_PORT, NULL, 0);

    ESP_LOGI(TAG, "WiFi init — STA → %s | AP fallback in %d s (per SSID)",
             MDNS_HOSTNAME, AP_FALLBACK_TIMEOUT_MS / 1000);
    return s_wifi_events;
}
