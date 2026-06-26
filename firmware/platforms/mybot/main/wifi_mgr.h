#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0

// Initializes WiFi + mDNS. Returns event group for WIFI_CONNECTED_BIT.
EventGroupHandle_t wifi_init(void);

#endif
