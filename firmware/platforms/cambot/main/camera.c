// main/camera.c
// ============================================================================
//  CamBot — OV2640 camera init + stream control
//
//  Uses the espressif/esp32-camera IDF component.
//  Frame buffers are allocated in PSRAM (CONFIG_SPIRAM=y required).
//
//  Pin assignments are taken from board_config.h (CAMERA_PIN_* macros).
// ============================================================================
#include "camera.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "CAM";

// Atomic streaming flag — written by /cam_on, /cam_off, and boot button.
// Read by the MJPEG stream handler.
static volatile bool s_streaming = false;

esp_err_t camera_init(void) {
    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_0,
        .ledc_timer   = LEDC_TIMER_0,
        .pin_d0       = CAMERA_PIN_D0,
        .pin_d1       = CAMERA_PIN_D1,
        .pin_d2       = CAMERA_PIN_D2,
        .pin_d3       = CAMERA_PIN_D3,
        .pin_d4       = CAMERA_PIN_D4,
        .pin_d5       = CAMERA_PIN_D5,
        .pin_d6       = CAMERA_PIN_D6,
        .pin_d7       = CAMERA_PIN_D7,
        .pin_xclk     = CAMERA_PIN_XCLK,
        .pin_pclk     = CAMERA_PIN_PCLK,
        .pin_vsync    = CAMERA_PIN_VSYNC,
        .pin_href     = CAMERA_PIN_HREF,
        .pin_sscb_sda = CAMERA_PIN_SIOD,
        .pin_sscb_scl = CAMERA_PIN_SIOC,
        .pin_pwdn     = CAMERA_PIN_PWDN,
        .pin_reset    = CAMERA_PIN_RESET,
        .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = CAMERA_FRAME_SIZE,
        .jpeg_quality = CAMERA_JPEG_QUALITY,
        .fb_count     = CAMERA_FB_COUNT,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return err;
    }

    // Optional: tweak sensor settings after init
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, CAMERA_FRAME_SIZE);
        s->set_quality(s, CAMERA_JPEG_QUALITY);
        // Flip image if camera is mounted upside-down:
        // s->set_vflip(s, 1);
        // s->set_hmirror(s, 1);
    }

    ESP_LOGI(TAG, "Camera init OK — %dx JPEG quality %d",
             CAMERA_FB_COUNT, CAMERA_JPEG_QUALITY);
    return ESP_OK;
}

bool camera_is_streaming(void) {
    return s_streaming;
}

void camera_set_streaming(bool on) {
    s_streaming = on;
    ESP_LOGI(TAG, "Stream %s", on ? "ON" : "OFF");
}
