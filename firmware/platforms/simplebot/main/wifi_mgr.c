#include "wifi_mgr.h"
#include "webserver.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>

#define TAG "WIFI"

// After this many ms without getting an IP, fall back to SoftAP mode.
#define AP_FALLBACK_MS  12000

static EventGroupHandle_t s_wifi_events;
static TimerHandle_t      s_ap_timer  = NULL;
static bool               s_ap_active = false;

// Multi-SSID tracking
static int s_network_idx = 0;
static const char* s_ssids[] = { WIFI_SSID_1, WIFI_SSID_2 };
static const char* s_passes[] = { WIFI_PASS_1, WIFI_PASS_2 };
#define NUM_NETWORKS (sizeof(s_ssids) / sizeof(s_ssids[0]))

// ── SoftAP setup ─────────────────────────────────────────────────────────────
// AP SSID: "MojDog-XX"  (XX = device number from board_config.h)
// AP IP:   192.168.4.1  (ESP32 default)
// No password — dogs are friendly.

static void start_softap(void) {
    if (s_ap_active) return;
    s_ap_active = true;

    ESP_LOGW(TAG, "STA failed — starting SoftAP at 192.168.4.1");

    // Create AP netif (STA netif was already created in wifi_init)
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = 0,
            .channel        = 6,
            .authmode       = WIFI_AUTH_OPEN,   // no password
            .max_connection = 4,
        },
    };
    // Build SSID "MojDog-<device_num>"
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid),
             "MojDog-%d", DEVICE_NUMBER);

    // Switch to APSTA so we still try STA in background
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    ESP_LOGI(TAG, "SoftAP started — SSID: %s  IP: 192.168.4.1",
             (char *)ap_cfg.ap.ssid);

    // Start the web server on the AP interface too
    webserver_start();
}

static void ap_fallback_cb(TimerHandle_t xTimer) {
    if (s_ap_active) return;

    s_network_idx++;
    if (s_network_idx < NUM_NETWORKS) {
        ESP_LOGW(TAG, "STA timeout — trying next SSID: %s", s_ssids[s_network_idx]);
        
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        strncpy((char*)wifi_config.sta.ssid, s_ssids[s_network_idx], sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, s_passes[s_network_idx], sizeof(wifi_config.sta.password));
        
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
        
        // Reset timer for the next network
        xTimerReset(s_ap_timer, 0);
    } else {
        start_softap();
    }
}

// ── ESP-NETIF style event handler ──
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *event_data) {
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        // Start the AP-fallback countdown
        if (s_ap_timer) {
            xTimerReset(s_ap_timer, 0);
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_ap_active) {
            ESP_LOGW(TAG, "Disconnected — retrying...");
            esp_wifi_connect();
            // Note: We DON'T reset the fallback timer here so that the 
            // SSID switch/AP fallback happens after AP_FALLBACK_MS total.
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
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // STA connected — cancel the AP fallback timer
        if (s_ap_timer) xTimerStop(s_ap_timer, 0);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        webserver_start();
    }
}

EventGroupHandle_t wifi_init(void) {
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // ESP-NETIF instance-based registration (modern IDF 5.x style)
    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, &h_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)wifi_config.sta.ssid, s_ssids[0], sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, s_passes[0], sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // mDNS (works regardless of STA/AP mode)
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE));

    // Create the AP-fallback timer (one-shot, fires after AP_FALLBACK_MS)
    s_ap_timer = xTimerCreate("ap_fallback",
                              pdMS_TO_TICKS(AP_FALLBACK_MS),
                              pdFALSE, NULL, ap_fallback_cb);

    ESP_LOGI(TAG, "WiFi init — STA → %s | AP fallback in %d s",
             MDNS_HOSTNAME, AP_FALLBACK_MS / 1000);
    return s_wifi_events;
}
