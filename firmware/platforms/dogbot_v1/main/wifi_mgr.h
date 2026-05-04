#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include <stdbool.h>

#define WIFI_CONNECTED_BIT BIT0

// Initializes WiFi + mDNS. Returns event group for WIFI_CONNECTED_BIT.
EventGroupHandle_t wifi_init(void);

// Returns true if we are currently in SoftAP (fallback) mode.
bool wifi_is_ap_mode(void);

// Save a new WiFi credential to NVS.  Returns ESP_OK on success.
// After saving, the caller should reboot to connect with the new creds.
esp_err_t wifi_save_credential(const char *ssid, const char *pass);

// Get the number of NVS-stored credentials.
int wifi_nvs_credential_count(void);

// Read credential N (0-indexed). Returns false if index out of range.
bool wifi_nvs_credential_get(int index, char *ssid, size_t ssid_len,
                              char *pass, size_t pass_len);

// Delete a single NVS-stored credential by index. Returns ESP_OK on success.
esp_err_t wifi_nvs_credential_delete(int index);

#endif
