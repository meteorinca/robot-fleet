#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

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

#endif
