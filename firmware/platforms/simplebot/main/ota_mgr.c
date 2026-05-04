// ota_mgr.c — Minimal single-bank OTA update manager
//
// Flow:
//   1. POST /ota begins streaming the .bin file body
//   2. HTTP handler calls ota_begin() once
//   3. HTTP handler calls ota_write() in chunks as data arrives
//   4. HTTP handler calls ota_end() when content_len bytes received
//   5. ota_end() validates the image + marks ota_0 as boot partition
//   6. HTTP handler sends "OK" response, then esp_restart()
//
// If an error occurs at any step, ota_abort() cleans up and the
// LED switches to OTA_STATE_FAILED for ~5 s then reverts to IDLE.

#include "ota_mgr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "OTA";

// Shared state read by the ws2812 heartbeat task
volatile ota_state_t g_ota_state = OTA_STATE_IDLE;

esp_err_t ota_begin(ota_handle_t *h) {
    if (!h) return ESP_ERR_INVALID_ARG;

    h->partition = esp_ota_get_next_update_partition(NULL);
    if (!h->partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Writing to partition '%s' at offset 0x%lx",
             h->partition->label, h->partition->address);

    esp_err_t err = esp_ota_begin(h->partition, OTA_WITH_SEQUENTIAL_WRITES, &h->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }
    g_ota_state = OTA_STATE_ACTIVE;
    return ESP_OK;
}

esp_err_t ota_write(ota_handle_t *h, const void *data, size_t len) {
    if (!h || !data || len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = esp_ota_write(h->handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ota_end(ota_handle_t *h) {
    if (!h) return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_ota_end(h->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (CRC?): %s", esp_err_to_name(err));
        g_ota_state = OTA_STATE_FAILED;
        return err;
    }

    err = esp_ota_set_boot_partition(h->partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        g_ota_state = OTA_STATE_FAILED;
        return err;
    }

    ESP_LOGI(TAG, "OTA complete — will restart");
    g_ota_state = OTA_STATE_SUCCESS;
    return ESP_OK;
}

void ota_abort(ota_handle_t *h) {
    if (h && h->handle) {
        esp_ota_abort(h->handle);
    }
    g_ota_state = OTA_STATE_FAILED;
    ESP_LOGW(TAG, "OTA aborted");
}
