#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define WIFI_CONNECTED_BIT BIT0

typedef enum {
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE
} wifi_state_t;

// Initializes WiFi + mDNS. Returns event group for WIFI_CONNECTED_BIT.
EventGroupHandle_t wifi_init(void);

wifi_state_t wifi_mgr_get_state(void);
const char* wifi_mgr_get_ip(void);
// Returns true if we are currently in SoftAP (fallback) mode.
bool wifi_is_ap_mode(void);

// Save a new WiFi credential to NVS. Returns ESP_OK on success.
// After saving, the caller should reboot to connect with the new creds.
esp_err_t wifi_save_credential(const char *ssid, const char *pass);

// Get the number of NVS-stored credentials.
int wifi_nvs_credential_count(void);

// Read credential N (0-indexed). Returns false if index out of range.
bool wifi_nvs_credential_get(int index, char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len);

// Delete a single NVS-stored credential by index. Returns ESP_OK on success.
esp_err_t wifi_nvs_credential_delete(int index);

// Erase ALL stored WiFi credentials and restart the device into AP/hotspot mode.
// This is the "factory-reset networking" function — irreversible without re-setup.
void wifi_forget_all(void);

#endif // WIFI_MGR_H
