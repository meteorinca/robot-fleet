#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
} ota_handle_t;

typedef enum {
    OTA_STATE_IDLE    = 0,
    OTA_STATE_ACTIVE  = 1,
    OTA_STATE_SUCCESS = 2,
    OTA_STATE_FAILED  = 3,
} ota_state_t;

extern volatile ota_state_t g_ota_state;

esp_err_t ota_begin(ota_handle_t *h);
esp_err_t ota_write(ota_handle_t *h, const void *data, size_t len);
esp_err_t ota_end(ota_handle_t *h);
void      ota_abort(ota_handle_t *h);
