#ifndef CAMERA_H
#define CAMERA_H

#include "esp_err.h"
#include "esp_camera.h"

// Initialize the OV2640 camera with board pins from board_config.h.
// Must be called after NVS init, before WiFi or webserver.
esp_err_t camera_init(void);

// Returns true if the MJPEG stream is currently active.
bool camera_is_streaming(void);

// Start or stop the MJPEG stream.
void camera_set_streaming(bool on);

#endif // CAMERA_H
