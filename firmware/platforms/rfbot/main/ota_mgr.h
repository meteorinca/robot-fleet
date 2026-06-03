#pragma once
// ota_mgr.h — Minimal single-bank OTA update manager
//
// Usage (called by the HTTP POST /ota handler):
//   ota_handle_t h;
//   ota_begin(&h);
//   while (more_data)  ota_write(&h, buf, len);
//   ota_end(&h);        // validates + sets boot flag; caller should restart

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
} ota_handle_t;

// LED state visible to ws2812 heartbeat task
typedef enum {
    OTA_STATE_IDLE    = 0,  // normal operation
    OTA_STATE_ACTIVE  = 1,  // flashing in progress → cyan strobe
    OTA_STATE_SUCCESS = 2,  // done → green triple-flash (then restart)
    OTA_STATE_FAILED  = 3,  // error → red fast blink (for ~5 s)
} ota_state_t;

// Read by ws2812 heartbeat to decide LED pattern
extern volatile ota_state_t g_ota_state;

esp_err_t ota_begin(ota_handle_t *h);
esp_err_t ota_write(ota_handle_t *h, const void *data, size_t len);
esp_err_t ota_end(ota_handle_t *h);   // validates CRC; sets boot partition
void      ota_abort(ota_handle_t *h); // cleans up on receive error
