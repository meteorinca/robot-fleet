// main/webserver.c
// ============================================================================
//  CamBot - RC Car HTTP Server (Camera + Motor Control)
//
//  Endpoints:
//    GET /            - Web UI (RC car controller + live MJPEG feed)
//    GET /stream      - MJPEG multipart stream (async task, max framerate)
//    GET /cam_on      - Start streaming -> {"streaming":true}
//    GET /cam_off     - Stop streaming  -> {"streaming":false}
//    GET /snapshot    - Single JPEG capture -> image/jpeg
//    GET /cam_status  - Query state -> {"streaming":true|false}
//    GET /drive       - Motor control: ?steer=<-100..100>&drive=<-100..100>
//                       or ?stop=1 for emergency stop -> {"steer":<n>,"drive":<n>}
//    GET /time        - NTP time JSON
//    GET /status      - Firmware status JSON
//    GET /schedule    - Schedule a named action (cam_on, cam_off, l1on, stop, ...)
//    GET /sync_time   - Set device clock from browser epoch
//    GET /l1on        - LED on
//    GET /l1off       - LED off
//    GET /toggle      - LED toggle
//    POST /ota        - OTA firmware upload
// ============================================================================

#include "webserver.h"
#include "config.h"
#include "camera.h"
#include "led.h"
#include "motor.h"
#include "timekeep.h"
#include "ota_mgr.h"
#include "esp_camera.h"
#include <string.h>
#include <stdio.h>
#include "esp_system.h"
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/socket.h>
#include <netinet/tcp.h>
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static httpd_handle_t s_server = NULL;

// ----------------------------------------------------------------------------
//  Live Stream Instrumentation & Camera Model Detection
// ----------------------------------------------------------------------------
typedef struct {
    bool     active;
    uint32_t frames_sent;
    uint64_t total_bytes;
    int64_t  start_time_us;
    int64_t  last_frame_time_us;
    uint32_t last_capture_us;
    uint32_t avg_capture_us;
    uint32_t max_capture_us;
    uint32_t min_capture_us;
    uint32_t last_send_us;
    uint32_t avg_send_us;
    uint32_t max_send_us;
    uint32_t min_send_us;
    uint32_t last_frame_bytes;
    uint32_t avg_frame_bytes;
    uint32_t send_errors;
    float    fps_current;
    float    bitrate_kbps;
} stream_stats_t;

static stream_stats_t s_stream_stats = {0};

static const char* get_camera_sensor_name(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return "Not Initialized";
    switch (s->id.PID) {
        case OV2640_PID: return "OV2640";
        case OV3660_PID: return "OV3660";
        case OV5640_PID: return "OV5640";
        case OV7725_PID: return "OV7725";
        case OV7670_PID: return "OV7670";
        case NT99141_PID: return "NT99141";
        case GC2145_PID: return "GC2145";
        case GC032A_PID: return "GC032A";
        case GC0308_PID: return "GC0308";
        case BF3005_PID: return "BF3005";
        case BF20A6_PID: return "BF20A6";
        case SC101IOT_PID: return "SC101IOT";
        case SC030IOT_PID: return "SC030IOT";
        case SC031GS_PID: return "SC031GS";
        default: return "Unknown Sensor";
    }
}

static int compare_u32(const void *a, const void *b) {
    uint32_t val_a = *(const uint32_t *)a;
    uint32_t val_b = *(const uint32_t *)b;
    return (val_a > val_b) - (val_a < val_b);
}

// ----------------------------------------------------------------------------
//  Named-action dispatcher (used by webserver + scheduler)
// ----------------------------------------------------------------------------
void execute_named_action(const char *action) {
    ESP_LOGI("ACTION", "Executing: %s", action);
    if      (strcmp(action, "cam_on")       == 0) camera_set_streaming(true);
    else if (strcmp(action, "cam_off")      == 0) camera_set_streaming(false);
    else if (strcmp(action, "l1on")         == 0 || strcmp(action, "flash_on")  == 0) led_action_set(true);
    else if (strcmp(action, "l1off")        == 0 || strcmp(action, "flash_off") == 0) led_action_set(false);
    else if (strcmp(action, "toggle")       == 0 || strcmp(action, "flash")     == 0 || strcmp(action, "flash_toggle") == 0 || strcmp(action, "light") == 0) led_action_toggle();
    else if (strcmp(action, "stop")         == 0) motor_stop_all();
    else ESP_LOGW("ACTION", "Unknown action: %s", action);
}

// ----------------------------------------------------------------------------
//  CORS + common helpers
// ----------------------------------------------------------------------------
static esp_err_t cors_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ════════════════════════════════════════════════════════════════
//  GET /stream - MJPEG multipart streaming (async task-based)
//
//  Architecture:
//    stream_handler() returns immediately via httpd_req_async_handler_begin(),
//    freeing the httpd worker thread so /cam_off and other APIs continue
//    to work while streaming. A dedicated FreeRTOS task owns the socket
//    and drives the capture loop until:
//      - camera_is_streaming() becomes false (/cam_off or boot button)
//      - the client disconnects (httpd_resp_send_chunk returns an error)
//
//  Only one streaming client is supported at a time (matches OV2640 DMA
//  architecture). A second /stream request gets a styled 503 page.
// ════════════════════════════════════════════════════════════════
#define MJPEG_BOUNDARY       "frame"
#define MJPEG_CONTENT_TYPE   "multipart/x-mixed-replace;boundary=" MJPEG_BOUNDARY

// NULL when no stream is active; non-NULL when a task owns the socket.
static TaskHandle_t s_stream_task = NULL;

static void mjpeg_stream_task(void *arg) {
    httpd_req_t *req = (httpd_req_t *)arg;
    esp_err_t res = ESP_OK;
    char part_buf[192];

    // Set TCP_NODELAY and socket send timeout to eliminate network buffering latency
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    ESP_LOGI("STREAM", "Streaming task started");

    // Initialize real-time stream stats
    s_stream_stats.active = true;
    s_stream_stats.frames_sent = 0;
    s_stream_stats.total_bytes = 0;
    s_stream_stats.start_time_us = esp_timer_get_time();
    s_stream_stats.last_frame_time_us = s_stream_stats.start_time_us;
    s_stream_stats.avg_capture_us = 0;
    s_stream_stats.max_capture_us = 0;
    s_stream_stats.min_capture_us = UINT32_MAX;
    s_stream_stats.avg_send_us = 0;
    s_stream_stats.max_send_us = 0;
    s_stream_stats.min_send_us = UINT32_MAX;
    s_stream_stats.send_errors = 0;
    s_stream_stats.fps_current = 0.0f;
    s_stream_stats.bitrate_kbps = 0.0f;

    // Flush any stale/accumulated frames in the hardware queue from when streaming was stopped
    for (int i = 0; i < 2; i++) {
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) {
            esp_camera_fb_return(stale);
        }
    }

    uint32_t frame_seq = 0;
    while (camera_is_streaming()) {
        int64_t t_cap_start = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        int64_t t_cap_end = esp_timer_get_time();
        uint32_t cap_us = (uint32_t)(t_cap_end - t_cap_start);

        if (!fb) {
            ESP_LOGE("STREAM", "Camera capture failed - stopping");
            res = ESP_FAIL;
            break;
        }

        frame_seq++;
        size_t frame_len = fb->len;

        // Part header with embedded diagnostic timing instrumentation
        size_t hlen = snprintf(part_buf, sizeof(part_buf),
            "--" MJPEG_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "X-Frame-Num: %lu\r\n"
            "X-Cap-Time-Us: %lu\r\n\r\n",
            frame_len,
            (unsigned long)frame_seq,
            (unsigned long)cap_us);

        int64_t t_send_start = esp_timer_get_time();
        res = httpd_resp_send_chunk(req, part_buf, (ssize_t)hlen);
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, (ssize_t)frame_len);

        // Return frame buffer immediately so camera DMA never starves
        esp_camera_fb_return(fb);
        fb = NULL;

        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, "\r\n", 2);

        int64_t t_send_end = esp_timer_get_time();
        uint32_t send_us = (uint32_t)(t_send_end - t_send_start);

        if (res != ESP_OK) {
            s_stream_stats.send_errors++;
            ESP_LOGI("STREAM", "Client disconnected (send error)");
            break;
        }

        // Update live metrics
        s_stream_stats.frames_sent++;
        s_stream_stats.total_bytes += frame_len + hlen + 2;
        s_stream_stats.last_capture_us = cap_us;
        s_stream_stats.last_send_us = send_us;
        s_stream_stats.last_frame_bytes = (uint32_t)frame_len;
        if (cap_us > s_stream_stats.max_capture_us) s_stream_stats.max_capture_us = cap_us;
        if (cap_us < s_stream_stats.min_capture_us) s_stream_stats.min_capture_us = cap_us;
        if (send_us > s_stream_stats.max_send_us) s_stream_stats.max_send_us = send_us;
        if (send_us < s_stream_stats.min_send_us) s_stream_stats.min_send_us = send_us;

        if (s_stream_stats.frames_sent == 1) {
            s_stream_stats.avg_capture_us = cap_us;
            s_stream_stats.avg_send_us = send_us;
            s_stream_stats.avg_frame_bytes = (uint32_t)frame_len;
        } else {
            s_stream_stats.avg_capture_us = (s_stream_stats.avg_capture_us * 7 + cap_us) / 8;
            s_stream_stats.avg_send_us = (s_stream_stats.avg_send_us * 7 + send_us) / 8;
            s_stream_stats.avg_frame_bytes = (s_stream_stats.avg_frame_bytes * 7 + (uint32_t)frame_len) / 8;
        }

        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - s_stream_stats.start_time_us;
        if (elapsed_us > 0) {
            s_stream_stats.fps_current = (float)s_stream_stats.frames_sent * 1000000.0f / (float)elapsed_us;
            s_stream_stats.bitrate_kbps = (float)(s_stream_stats.total_bytes * 8) * 1000.0f / (float)elapsed_us;
        }
        s_stream_stats.last_frame_time_us = now_us;
    }

    // Send closing boundary then terminate the chunked response
    httpd_resp_send_chunk(req, "--" MJPEG_BOUNDARY "--\r\n", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);

    // Release the async request slot back to httpd
    httpd_req_async_handler_complete(req);

    s_stream_stats.active = false;
    ESP_LOGI("STREAM", "Streaming task ended (%lu frames sent, avg cap=%.1f ms, avg send=%.1f ms)",
             (unsigned long)s_stream_stats.frames_sent,
             s_stream_stats.avg_capture_us / 1000.0f,
             s_stream_stats.avg_send_us / 1000.0f);
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // ------------------------------------------------------------------------
    if (s_stream_task != NULL) {
        httpd_resp_set_status(req, "503 Stream Busy");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            "<!DOCTYPE html><html>"
            "<head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>CamBot &mdash; Stream Busy</title>"
            "<style>body{font-family:system-ui,sans-serif;background:#080810;color:#e0e0f0;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}"
            ".box{text-align:center;padding:32px;background:#0f0f20;border:1px solid #1e1e38;"
            "border-radius:16px;max-width:360px;}"
            "h2{font-size:20px;color:#f7736a;margin-bottom:10px;}"
            "p{color:#888;font-size:14px;line-height:1.5;}"
            "a{color:#7c6af7;text-decoration:none;font-weight:600;}"
            "</style></head><body>"
            "<div class='box'>"
            "<h2>&#x1F4E1; Stream Busy</h2>"
            "<p>Another device is already receiving the stream.<br>"
            "Only one simultaneous viewer is supported.</p>"
            "<br><a href='/'>&#x2190; Back to UI</a>"
            "</div>"
            "</body></html>",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // -- Stream is off --------------------------------------------------------
    if (!camera_is_streaming()) {
        httpd_resp_set_status(req, "503 Stream Off");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            "<!DOCTYPE html><html>"
            "<head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>CamBot - Stream Off</title>"
            "<style>body{font-family:system-ui,sans-serif;background:#080810;color:#e0e0f0;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}"
            ".box{text-align:center;padding:32px;background:#0f0f20;border:1px solid #1e1e38;"
            "border-radius:16px;max-width:360px;}"
            "h2{font-size:20px;color:#888;margin-bottom:10px;}"
            "p{color:#666;font-size:14px;}"
            "a{color:#00e5a0;text-decoration:none;font-weight:600;}"
            "</style></head><body>"
            "<div class='box'>"
            "<h2>&#x23F8; Stream is Off</h2>"
            "<p>Press <strong>Stream ON</strong> in the UI to start.</p>"
            "<br><a href='/'>&#x2190; Open UI</a>"
            "</div>"
            "</body></html>",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // -- Start async streaming ------------------------------------------------
    httpd_resp_set_type(req, MJPEG_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "Connection", "close");

    // httpd_req_async_handler_begin() detaches this request from the httpd
    // worker thread so the server remains responsive for /cam_off, /status, etc.
    httpd_req_t *async_req;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        ESP_LOGE("STREAM", "Failed to begin async handler");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Async init failed");
        return ESP_FAIL;
    }

    // 4 KB stack pinned to Core 1 - frame processing is minimal; actual JPEG data is in PSRAM.
    // Core 1 handles camera DMA/capture while Core 0 handles WiFi MAC/LWIP stack concurrently.
    if (xTaskCreatePinnedToCore(mjpeg_stream_task, "mjpeg_stream", 4096,
                                async_req, 5, &s_stream_task, 1) != pdPASS) {
        ESP_LOGE("STREAM", "Failed to create streaming task");
        httpd_req_async_handler_complete(async_req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI("STREAM", "Async streaming task launched");
    return ESP_OK;  // httpd worker is now FREE - /cam_off and other APIs work normally
}

// ----------------------------------------------------------------------------
//  GET /diag - Deep Diagnostic and Camera Benchmark
// ----------------------------------------------------------------------------
static esp_err_t diag_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "application/json");

    int frames_to_test = 20;
    bool do_bench = true;
    char qs[64];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(qs, "frames", p, sizeof(p)) == ESP_OK) {
            frames_to_test = atoi(p);
            if (frames_to_test < 0) frames_to_test = 0;
            if (frames_to_test > 50) frames_to_test = 50;
        }
        if (httpd_query_key_value(qs, "bench", p, sizeof(p)) == ESP_OK) {
            if (atoi(p) == 0) do_bench = false;
        }
    }

    // System info
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    wifi_ap_record_t ap_info = {0};
    int8_t rssi = 0;
    uint8_t channel = 0;
    char ssid[33] = "disconnected";
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
        channel = ap_info.primary;
        strncpy(ssid, (char *)ap_info.ssid, sizeof(ssid) - 1);
    }

    sensor_t *s = esp_camera_sensor_get();
    const char *sensor_name = get_camera_sensor_name();
    uint16_t sensor_pid = s ? s->id.PID : 0;
    int framesize = s ? s->status.framesize : -1;
    int quality = s ? s->status.quality : -1;
    bool currently_streaming = camera_is_streaming() || (s_stream_task != NULL);

    // On-device capture benchmark (only if stream is NOT running to avoid sensor lock contention)
    uint32_t cap_times_us[50];
    uint32_t sorted_caps[50];
    uint32_t frame_sizes[50];
    int frames_captured = 0;
    uint64_t total_cap_us = 0;
    uint64_t total_bytes = 0;
    uint32_t min_cap = UINT32_MAX;
    uint32_t max_cap = 0;
    uint32_t min_size = UINT32_MAX;
    uint32_t max_size = 0;

    int64_t bench_start_us = esp_timer_get_time();
    if (do_bench && frames_to_test > 0 && !currently_streaming) {
        for (int i = 0; i < frames_to_test; i++) {
            int64_t t0 = esp_timer_get_time();
            camera_fb_t *fb = esp_camera_fb_get();
            int64_t t1 = esp_timer_get_time();
            if (!fb) break;

            uint32_t dt = (uint32_t)(t1 - t0);
            uint32_t sz = (uint32_t)fb->len;
            esp_camera_fb_return(fb);

            cap_times_us[frames_captured] = dt;
            sorted_caps[frames_captured] = dt;
            frame_sizes[frames_captured] = sz;
            total_cap_us += dt;
            total_bytes += sz;
            if (dt < min_cap) min_cap = dt;
            if (dt > max_cap) max_cap = dt;
            if (sz < min_size) min_size = sz;
            if (sz > max_size) max_size = sz;
            frames_captured++;
        }
    }
    int64_t bench_total_us = esp_timer_get_time() - bench_start_us;

    float avg_cap_ms = 0.0f;
    float p95_cap_ms = 0.0f;
    float raw_fps = 0.0f;
    float avg_size_bytes = 0.0f;

    if (frames_captured > 0) {
        avg_cap_ms = (float)total_cap_us / (float)(frames_captured * 1000.0f);
        avg_size_bytes = (float)total_bytes / (float)frames_captured;
        qsort(sorted_caps, frames_captured, sizeof(uint32_t), compare_u32);
        int p95_idx = (frames_captured * 95) / 100;
        if (p95_idx >= frames_captured) p95_idx = frames_captured - 1;
        p95_cap_ms = (float)sorted_caps[p95_idx] / 1000.0f;
        if (bench_total_us > 0) {
            raw_fps = (float)frames_captured * 1000000.0f / (float)bench_total_us;
        }
    } else {
        min_cap = 0;
        min_size = 0;
    }

    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Alloc failed");
        return ESP_FAIL;
    }

    int len = snprintf(buf, 2048,
        "{\n"
        "  \"device\":\"%s\",\n"
        "  \"device_number\":%d,\n"
        "  \"version\":\"%s\",\n"
        "  \"sensor\":{\n"
        "    \"name\":\"%s\",\n"
        "    \"pid\":\"0x%04X\",\n"
        "    \"framesize\":%d,\n"
        "    \"quality\":%d,\n"
        "    \"xclk_mhz\":%d,\n"
        "    \"fb_count\":%d\n"
        "  },\n"
        "  \"system\":{\n"
        "    \"free_heap_bytes\":%lu,\n"
        "    \"min_free_heap_bytes\":%lu,\n"
        "    \"free_psram_bytes\":%lu,\n"
        "    \"min_free_psram_bytes\":%lu,\n"
        "    \"wifi_ssid\":\"%s\",\n"
        "    \"wifi_rssi_dbm\":%d,\n"
        "    \"wifi_channel\":%d,\n"
        "    \"streaming_active\":%s\n"
        "  },\n"
        "  \"capture_bench\":{\n"
        "    \"tested\":%s,\n"
        "    \"frames_requested\":%d,\n"
        "    \"frames_captured\":%d,\n"
        "    \"total_time_ms\":%.2f,\n"
        "    \"raw_fps\":%.2f,\n"
        "    \"capture_ms\":{\"avg\":%.2f,\"min\":%.2f,\"max\":%.2f,\"p95\":%.2f},\n"
        "    \"frame_bytes\":{\"avg\":%.1f,\"min\":%lu,\"max\":%lu}\n"
        "  },\n"
        "  \"live_stream\":{\n"
        "    \"active\":%s,\n"
        "    \"frames_sent\":%lu,\n"
        "    \"total_bytes\":%llu,\n"
        "    \"fps_current\":%.2f,\n"
        "    \"bitrate_kbps\":%.1f,\n"
        "    \"avg_capture_ms\":%.2f,\n"
        "    \"max_capture_ms\":%.2f,\n"
        "    \"min_capture_ms\":%.2f,\n"
        "    \"avg_send_ms\":%.2f,\n"
        "    \"max_send_ms\":%.2f,\n"
        "    \"min_send_ms\":%.2f,\n"
        "    \"send_errors\":%lu\n"
        "  }\n"
        "}\n",
        MDNS_HOSTNAME,
        DEVICE_NUMBER,
        FW_VERSION,
        sensor_name,
        sensor_pid,
        framesize,
        quality,
        CAMERA_XCLK_FREQ_HZ / 1000000,
        CAMERA_FB_COUNT,
        (unsigned long)free_heap,
        (unsigned long)min_free_heap,
        (unsigned long)free_psram,
        (unsigned long)min_free_psram,
        ssid,
        (int)rssi,
        (int)channel,
        currently_streaming ? "true" : "false",
        (frames_captured > 0) ? "true" : "false",
        frames_to_test,
        frames_captured,
        (float)bench_total_us / 1000.0f,
        raw_fps,
        avg_cap_ms,
        (float)min_cap / 1000.0f,
        (float)max_cap / 1000.0f,
        p95_cap_ms,
        avg_size_bytes,
        (unsigned long)min_size,
        (unsigned long)max_size,
        s_stream_stats.active ? "true" : "false",
        (unsigned long)s_stream_stats.frames_sent,
        (unsigned long long)s_stream_stats.total_bytes,
        s_stream_stats.fps_current,
        s_stream_stats.bitrate_kbps,
        (float)s_stream_stats.avg_capture_us / 1000.0f,
        (float)s_stream_stats.max_capture_us / 1000.0f,
        (s_stream_stats.min_capture_us == UINT32_MAX) ? 0.0f : ((float)s_stream_stats.min_capture_us / 1000.0f),
        (float)s_stream_stats.avg_send_us / 1000.0f,
        (float)s_stream_stats.max_send_us / 1000.0f,
        (s_stream_stats.min_send_us == UINT32_MAX) ? 0.0f : ((float)s_stream_stats.min_send_us / 1000.0f),
        (unsigned long)s_stream_stats.send_errors
    );

    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}



// ----------------------------------------------------------------------------
//  GET /cam_on  /cam_off  /cam_status
// ----------------------------------------------------------------------------
static esp_err_t cam_on_handler(httpd_req_t *req) {
    camera_set_streaming(true);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"streaming\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cam_off_handler(httpd_req_t *req) {
    camera_set_streaming(false);
    // Wait up to 200ms for active stream task to cleanly exit and close socket
    int wait_ms = 0;
    while (s_stream_task != NULL && wait_ms < 200) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait_ms += 20;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"streaming\":false}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cam_status_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    bool s = camera_is_streaming();
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"streaming\":%s}", s ? "true" : "false");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET /snapshot  single JPEG frame
// ----------------------------------------------------------------------------
static esp_err_t snapshot_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
        return ESP_FAIL;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
    httpd_resp_send(req, (const char *)fb->buf, (ssize_t)fb->len);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET /time  /status  /sync_time  /schedule
// ----------------------------------------------------------------------------
static esp_err_t time_handler(httpd_req_t *req) {
    char buf[32];
    timekeep_format(buf, sizeof(buf));
    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"formatted\":\"%s\",\"epoch\":%lld,\"synced\":%s}",
        buf, (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
    char resp[192];
    int len = snprintf(resp, sizeof(resp),
        "{\"status\":\"running\",\"version\":\"%s\",\"epoch\":%lld,"
        "\"time_synced\":%s,\"streaming\":%s}",
        FW_VERSION, (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false",
        camera_is_streaming() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t sync_time_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "epoch", param, sizeof(param)) == ESP_OK) {
            time_t t = strtol(param, NULL, 10);
            timekeep_set_time(t);
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t schedule_handler(httpd_req_t *req) {
    char qs[200];
    char action[32] = {0};
    int  delay_sec  = 0;
    int  delay_ms   = 0;
    int  extra_ms   = 0;
    time_t at       = 0;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char p[64];
        if (httpd_query_key_value(qs, "action",   p, sizeof(p)) == ESP_OK)
            strncpy(action, p, sizeof(action) - 1);
        if (httpd_query_key_value(qs, "at",       p, sizeof(p)) == ESP_OK)
            at = (time_t)strtol(p, NULL, 10);
        if (httpd_query_key_value(qs, "ms",       p, sizeof(p)) == ESP_OK)
            extra_ms = atoi(p);
        if (httpd_query_key_value(qs, "delay",    p, sizeof(p)) == ESP_OK)
            delay_sec = atoi(p);
        if (httpd_query_key_value(qs, "delay_ms", p, sizeof(p)) == ESP_OK)
            delay_ms  = atoi(p);
    }

    if (!action[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing ?action=\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (at == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        at       = tv.tv_sec + delay_sec + (delay_ms / 1000);
        extra_ms = (tv.tv_usec / 1000) + (delay_ms % 1000);
        if (extra_ms >= 1000) { at++; extra_ms -= 1000; }
    }

    timekeep_schedule_ms(action, at, extra_ms);

    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"action\":\"%s\",\"at\":%lld,\"ms\":%d}",
        action, (long long)at, extra_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

// ----------------------------------------------------------------------------
//  Quick-action handler (LED control, cam_on, cam_off)
// ----------------------------------------------------------------------------
static esp_err_t quick_action_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    if (uri[0] == '/') uri++;
    execute_named_action(uri);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ----------------------------------------------------------------------------
//  GET /drive  Motor control endpoint
// ----------------------------------------------------------------------------
//
//  Query parameters (all optional):
//    steer=<-100..100>   Steering: negative=right, positive=left, 0=centre
//    drive=<-100..100>   Drive:    negative=reverse, positive=forward, 0=stop
//    stop=1              Emergency stop (overrides steer/drive)
//
//  Returns JSON: {"steer":<n>,"drive":<n>}
// ----------------------------------------------------------------------------
static esp_err_t drive_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    char qs[128];
    char p[16];
    bool do_stop = false;

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        if (httpd_query_key_value(qs, "stop", p, sizeof(p)) == ESP_OK && atoi(p) != 0) {
            do_stop = true;
        }
        if (!do_stop) {
            if (httpd_query_key_value(qs, "steer", p, sizeof(p)) == ESP_OK)
                motor_steer(atoi(p));
            if (httpd_query_key_value(qs, "drive", p, sizeof(p)) == ESP_OK)
                motor_drive(atoi(p));
        }
    }
    if (do_stop) motor_stop_all();

    char resp[48];
    int len = snprintf(resp, sizeof(resp),
        "{\"steer\":%d,\"drive\":%d}",
        motor_get_steer(), motor_get_drive());
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}


// ----------------------------------------------------------------------------
//  OTA POST /ota
// ----------------------------------------------------------------------------
#define OTA_BUF_SIZE 4096


static esp_err_t ota_post_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data");
        return ESP_FAIL;
    }
    ESP_LOGI("OTA", "Incoming firmware: %d bytes", total);

    ota_handle_t h = {0};
    if (ota_begin(&h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    static char buf[OTA_BUF_SIZE];
    int remaining = total;
    while (remaining > 0) {
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) {
            ESP_LOGE("OTA", "recv error (%d), aborting", received);
            ota_abort(&h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        if (ota_write(&h, buf, received) != ESP_OK) {
            ota_abort(&h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write error");
            return ESP_FAIL;
        }
        remaining -= received;
        if (((total - remaining) % 65536) < OTA_BUF_SIZE) {
            ESP_LOGI("OTA", "Progress: %d / %d bytes", total - remaining, total);
        }
    }

    if (ota_end(&h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ota\":\"ok\",\"restart\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

// ----------------------------------------------------------------------------
//  Root GET /  CamBot RC Car Controller UI
// ----------------------------------------------------------------------------
static esp_err_t root_get_handler(httpd_req_t *req) {
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang='en'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<meta name='apple-mobile-web-app-capable' content='yes'>"
        "<meta name='description' content='CamBot RC Car — live camera control from your browser'>"
        "<title>CamBot RC</title>"
        "<style>"
        "@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700;800&display=swap');"
        "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent;}"
        "html,body{min-height:100%;font-family:'Inter',system-ui,sans-serif,'Apple Color Emoji','Segoe UI Emoji','Noto Color Emoji';background:#07070f;color:#e0e0f0;overflow-x:hidden;}"
        /* Layout: left=camera+controls, right=joystick on desktop; stacked on mobile */
        ".layout{display:flex;min-height:100vh;gap:0;}"
        ".left{display:flex;flex-direction:column;flex:1;min-width:0;gap:0;}"
        /* Camera */
        ".cam-wrap{position:relative;width:100%;flex:1;min-height:260px;background:#000;display:flex;align-items:center;justify-content:center;overflow:hidden;}"
        "#cam-img{width:100%;height:100%;object-fit:contain;display:none;}"
        "#cam-ph{display:flex;flex-direction:column;align-items:center;gap:10px;color:#282840;}"
        "#cam-ph svg{width:64px;height:64px;}"
        "#cam-ph p{font-size:13px;color:#383858;}"
        /* Top HUD bar */
        ".hud{display:flex;align-items:center;gap:10px;padding:8px 14px;background:rgba(7,7,15,.85);backdrop-filter:blur(8px);border-bottom:1px solid #151528;flex-shrink:0;}"
        ".hud h1{font-size:15px;font-weight:800;letter-spacing:.5px;background:linear-gradient(135deg,#7c6af7,#00e5a0);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;flex:1;}"
        ".pill{padding:3px 10px;border-radius:20px;font-size:10px;font-weight:700;letter-spacing:.5px;}"
        ".pill.off{background:#151528;color:#444;}"
        ".pill.live{background:rgba(0,229,160,.12);color:#00e5a0;border:1px solid rgba(0,229,160,.25);}"
        ".dot{display:inline-block;width:6px;height:6px;border-radius:50%;margin-right:4px;background:#333;}"
        ".dot.live{background:#00e5a0;box-shadow:0 0 6px #00e5a0;animation:pulse 1.5s infinite;}"
        "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}"
        /* Bottom control bar */
        ".ctrl-bar{display:flex;align-items:center;gap:8px;padding:8px 10px;background:rgba(7,7,15,.9);border-top:1px solid #151528;flex-shrink:0;overflow-x:auto;}"
        ".cb{padding:7px 12px;border-radius:9px;border:1px solid #1e1e38;background:#0d0d1e;color:#a0a0d0;font-size:11px;font-weight:700;cursor:pointer;white-space:nowrap;transition:all .15s;letter-spacing:.3px;}"
        ".cb:hover{border-color:#7c6af7;color:#c0b0ff;}"
        ".cb:active{transform:scale(.96);}"
        ".cb.green{border-color:#00e5a0;color:#00e5a0;}"
        ".cb.red{border-color:#f7736a;color:#f7736a;}"
        ".cb.danger{border-color:#ff4444;color:#ff4444;background:rgba(255,68,68,.08);}"
        ".clock-sm{font-size:11px;font-family:monospace;color:#00e5a0;margin-left:auto;flex-shrink:0;}"
        /* Joystick panel */
        ".joy-panel{width:260px;flex-shrink:0;display:flex;flex-direction:column;align-items:center;gap:14px;"
        "background:linear-gradient(160deg,#0c0c1e,#0a0a18);border-left:1px solid #151528;padding:16px 14px;overflow-y:auto;}"
        ".joy-label{font-size:9px;font-weight:700;letter-spacing:1.5px;color:#333350;text-transform:uppercase;}"
        /* Joystick circle */
        ".joy-outer{position:relative;width:160px;height:160px;border-radius:50%;touch-action:none;"
        "background:radial-gradient(circle at 50% 50%,#0f0f22,#080814);"
        "border:2px solid #1e1e38;box-shadow:0 0 24px rgba(124,106,247,.1);}"
        ".joy-ring{position:absolute;inset:8px;border-radius:50%;border:1px solid #1e1e38;}"
        ".joy-cross-h{position:absolute;top:50%;left:0;right:0;height:1px;background:#151528;transform:translateY(-50%);}"
        ".joy-cross-v{position:absolute;left:50%;top:0;bottom:0;width:1px;background:#151528;transform:translateX(-50%);}"
        ".joy-knob{position:absolute;width:56px;height:56px;border-radius:50%;touch-action:none;"
        "background:radial-gradient(circle at 40% 40%,#9c8dff,#5b4de8);"
        "box-shadow:0 4px 20px rgba(124,106,247,.5);"
        "top:50%;left:50%;transform:translate(-50%,-50%);"
        "cursor:grab;transition:box-shadow .15s;}"
        ".joy-knob:active{cursor:grabbing;box-shadow:0 4px 32px rgba(124,106,247,.9);}"
        /* Direction indicators */
        ".dir-row{display:flex;gap:6px;}"
        ".dir-btn{width:44px;height:44px;border-radius:10px;border:1px solid #1e1e38;background:#0d0d1e;"
        "color:#555;font-size:18px;display:flex;align-items:center;justify-content:center;cursor:pointer;"
        "transition:all .1s;user-select:none;-webkit-user-select:none;}"
        ".dir-btn:active,.dir-btn.active{background:rgba(124,106,247,.2);border-color:#7c6af7;color:#c0b0ff;}"
        /* Motor readout */
        ".motor-row{display:flex;gap:8px;width:100%;}"
        ".motor-card{flex:1;background:#0a0a18;border:1px solid #151528;border-radius:10px;padding:8px 10px;text-align:center;}"
        ".motor-card .lbl{font-size:9px;color:#333;font-weight:700;letter-spacing:1px;text-transform:uppercase;margin-bottom:4px;}"
        ".motor-card .val{font-size:22px;font-weight:700;font-family:monospace;color:#7c6af7;}"
        /* WASD badge */
        ".wasd-hint{font-size:9px;color:#222240;text-align:center;}"
        /* OTA drawer */
        ".ota-drawer{display:none;position:fixed;inset:0;z-index:100;background:rgba(0,0,0,.7);align-items:center;justify-content:center;}"
        ".ota-box{background:#0f0f20;border:1px solid #1e1e38;border-radius:18px;padding:24px;width:340px;max-width:92vw;}"
        ".ota-box h3{font-size:14px;font-weight:700;color:#7c6af7;margin-bottom:14px;}"
        ".prog-wrap{background:#0a0a18;border-radius:6px;height:5px;margin:10px 0;overflow:hidden;}"
        "#ota-bar{width:0;height:100%;background:linear-gradient(90deg,#00e5a0,#7c6af7);transition:width .3s;}"
        ".ota-msg{font-size:12px;color:#666;margin-top:6px;}"
        ".btn-primary{width:100%;padding:11px;border:none;border-radius:10px;background:linear-gradient(135deg,#7c6af7,#5b4de8);color:#fff;font-size:13px;font-weight:700;cursor:pointer;margin-top:8px;}"
        ".btn-close{float:right;background:none;border:none;color:#555;font-size:18px;cursor:pointer;line-height:1;}"
        /* Responsive: on narrow screens hide joy-panel and show bottom arrows */
        "@media(max-width:600px){"
        ".layout{flex-direction:column;}"
        ".joy-panel{width:100%;border-left:none;border-top:1px solid #151528;padding:14px;}"
        ".joy-outer{width:130px;height:130px;}"
        ".joy-knob{width:46px;height:46px;}"
        "}"
        "</style>"
        "</head><body>"

        /* OTA drawer overlay */
        "<div class='ota-drawer' id='ota-drawer' onclick='if(event.target===this)closeOTA()'>"
        "<div class='ota-box'>"
        "<button class='btn-close' onclick='closeOTA()'>&#x2715;</button>"
        "<h3>&#x2B06; OTA Firmware Update</h3>"
        "<input type='file' id='ota-file' accept='.bin' style='color:#a0a0d0;margin-bottom:8px;width:100%;'>"
        "<div class='prog-wrap'><div id='ota-bar'></div></div>"
        "<div class='ota-msg' id='ota-msg'></div>"
        "<button class='btn-primary' onclick='doOTA()'>Flash Firmware</button>"
        "</div></div>"

        "<div class='layout'>"

        /* Left column */
        "<div class='left'>"
        /* HUD */
        "<div class='hud'>"
        "<h1>&#x1F916; CamBot RC</h1>"
        "<span class='pill off' id='stream-pill'><span class='dot' id='stream-dot'></span>Idle</span>"
        "</div>"

        /* Camera */
        "<div class='cam-wrap'>"
        "<img id='cam-img' alt='Live camera feed' onerror='onStreamError()'>"
        "<div id='cam-ph'>"
        "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1'>"
        "<path d='M15 10l4.553-2.277A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M3 8a2 2 0 012-2h10a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2V8z'/>"
        "</svg>"
        "<p>Press <strong>CAM ON</strong> to start</p>"
        "</div>"
        "</div>"

        /* Control bar */
        "<div class='ctrl-bar'>"
        "<button class='cb green' id='btn-cam-on'  onclick='streamOn()'>&#x25B6; CAM ON</button>"
        "<button class='cb red'   id='btn-cam-off' onclick='streamOff()' style='display:none'>&#x23F9; CAM OFF</button>"
        "<button class='cb' onclick='takeSnapshot()'>&#x1F4F8; Snap</button>"
        "<button class='cb' onclick='fetch(\"/toggle\")'>&#x1F4A1; LED</button>"
        "<button class='cb danger' id='btn-stop' onclick='eStop()'>&#x26D4; STOP</button>"
        "<button class='cb' onclick='openOTA()' style='margin-left:4px'>&#x2B06; OTA</button>"
        "<span class='clock-sm' id='clock'>--:--:--</span>"
        "</div>"
        "</div>"

        /* Right column - joystick & OTA */
        "<div class='joy-panel'>"
        "<div class='joy-label'>Steering &amp; Drive</div>"

        "<div class='joy-outer' id='joy-outer'>"
        "<div class='joy-ring'></div>"
        "<div class='joy-cross-h'></div>"
        "<div class='joy-cross-v'></div>"
        "<div class='joy-knob' id='joy-knob'></div>"
        "</div>"

        /* D-pad arrows */
        "<div style='display:flex;flex-direction:column;align-items:center;gap:5px;'>"
        "<div class='dir-row'>"
        "<div class='dir-btn' id='d-up' ontouchstart='kDown(\"w\")' ontouchend='kUp(\"w\")'>&#x25B2;</div>"
        "</div>"
        "<div class='dir-row'>"
        "<div class='dir-btn' id='d-left'  ontouchstart='kDown(\"a\")' ontouchend='kUp(\"a\")'>&#x25C0;</div>"
        "<div class='dir-btn' id='d-stop'  ontouchstart='eStop()'>&#x25A0;</div>"
        "<div class='dir-btn' id='d-right' ontouchstart='kDown(\"d\")' ontouchend='kUp(\"d\")'>&#x25B6;</div>"
        "</div>"
        "<div class='dir-row'>"
        "<div class='dir-btn' id='d-down' ontouchstart='kDown(\"s\")' ontouchend='kUp(\"s\")'>&#x25BC;</div>"
        "</div>"
        "</div>"

        /* Motor readout */
        "<div class='motor-row'>"
        "<div class='motor-card'><div class='lbl'>Steer</div><div class='val' id='sv'>0</div></div>"
        "<div class='motor-card'><div class='lbl'>Drive</div><div class='val' id='dv'>0</div></div>"
        "</div>"

        "<div class='speed-wrap' style='width:100%; padding-top:4px;'>"
        "<div class='joy-label' style='margin-bottom:6px'>Max Speed: <span id='spd-val'>50</span>%</div>"
        "<input type='range' id='spd-slider' min='10' max='100' value='50' style='width:100%; cursor:pointer;' oninput='document.getElementById(\"spd-val\").textContent=this.value; maxSpeed=this.value/100;'>"
        "</div>"

        "<div class='wasd-hint'>WASD / Arrow keys &middot; Gamepad supported</div>"

        "</div>" /* end .joy-panel */

        "</div>" /* end .layout */

        "<script>"
        /* ── State ── */
        "var streaming=false;"
        "var joyActive=false,joyX=0,joyY=0;"
        "var keys={};"
        "var gpTimer=null;"
        "var sendTimer=null;"
        "var lastSteer=0,lastDrive=0;"
        "var maxSpeed=0.5;"
        "var MAX_R=52;" /* joystick radius in px */

        /* ── Stream ── */
        "function setStreamingUI(on){"
        "  streaming=on;"
        "  var img=document.getElementById('cam-img');"
        "  var ph=document.getElementById('cam-ph');"
        "  var pill=document.getElementById('stream-pill');"
        "  var on_btn=document.getElementById('btn-cam-on');"
        "  var off_btn=document.getElementById('btn-cam-off');"
        "  if(on){"
        "    img.style.display='block';ph.style.display='none';"
        "    pill.className='pill live';"
        "    pill.innerHTML='<span class=\"dot live\"></span>LIVE';"
        "    on_btn.style.display='none';off_btn.style.display='block';"
        "  }else{"
        "    img.src='';img.style.display='none';ph.style.display='flex';"
        "    pill.className='pill off';"
        "    pill.innerHTML='<span class=\"dot\"></span>Idle';"
        "    on_btn.style.display='block';off_btn.style.display='none';"
        "  }"
        "}"
        "function streamOn(){"
        "  fetch('/cam_on').then(function(){"
        "    setStreamingUI(true);"
        "    var img=document.getElementById('cam-img');"
        "    img.src='/stream?t='+Date.now();"
        "  }).catch(function(){});"
        "}"
        "function streamOff(){"
        "  var img=document.getElementById('cam-img');"
        "  img.src='';"
        "  setStreamingUI(false);"
        "  fetch('/cam_off').catch(function(){});"
        "}"
        "function onStreamError(){"
        "  console.warn('Stream error — retrying');"
        "  if(streaming){"
        "    setTimeout(function(){"
        "      if(streaming){"
        "        var img=document.getElementById('cam-img');"
        "        img.src='/stream?t='+Date.now();"
        "      }"
        "    },1000);"
        "  }"
        "}"
        "function takeSnapshot(){"
        "  var a=document.createElement('a');a.href='/snapshot?t='+Date.now();"
        "  a.download='cambot.jpg';a.click();"
        "}"
        "setInterval(function(){"
        "  fetch('/cam_status').then(function(r){return r.json();})"
        "  .then(function(d){if(d.streaming&&!streaming)setStreamingUI(true);if(!d.streaming&&streaming)setStreamingUI(false);})"
        "  .catch(function(){});"
        "},10000);"

        /* ── Motor send (in-flight tracking + 25 Hz max rate limit) ── */
        "var driveSending=false,pendingDrive=null,lastDriveTime=0;"
        "function sendDrive(s,d){"
        "  s=Math.round(s*maxSpeed);d=Math.round(d*maxSpeed);"
        "  if(s===lastSteer&&d===lastDrive&&!pendingDrive)return;"
        "  document.getElementById('sv').textContent=s;"
        "  document.getElementById('dv').textContent=d;"
        "  var now=Date.now();"
        "  if(!driveSending&&(now-lastDriveTime>=40)){"
        "    lastDriveTime=now;driveSending=true;lastSteer=s;lastDrive=d;pendingDrive=null;"
        "    fetch('/drive?steer='+s+'&drive='+d,{cache:'no-store'}).finally(function(){"
        "      driveSending=false;"
        "      if(pendingDrive){var p=pendingDrive;pendingDrive=null;sendDrive(p.s,p.d);}"
        "    }).catch(function(){});"
        "  }else{pendingDrive={s:s,d:d};}"
        "}"
        "function eStop(){"
        "  joyX=0;joyY=0;keys={};pendingDrive=null;"
        "  moveKnob(0,0);"
        "  lastSteer=0;lastDrive=0;"
        "  document.getElementById('sv').textContent='0';"
        "  document.getElementById('dv').textContent='0';"
        "  fetch('/drive?stop=1',{cache:'no-store'}).catch(function(){});"
        "}"

        /* ── Joystick ── */
        "var jOuter=document.getElementById('joy-outer');"
        "var jKnob=document.getElementById('joy-knob');"
        "function joyRect(){return jOuter.getBoundingClientRect();}"
        "function moveKnob(nx,ny){"   /* nx,ny: -1..1 normalised */
        "  var r=joyRect();"
        "  var half=r.width/2;"
        "  MAX_R=half*0.62;"
        "  var px=nx*MAX_R;var py=ny*MAX_R;"
        "  jKnob.style.transform='translate(calc(-50% + '+px+'px),calc(-50% + '+py+'px))';"
        "}"
        "function joyUpdate(cx,cy){"
        "  var r=joyRect();"
        "  var ox=cx-r.left-r.width/2;"
        "  var oy=cy-r.top-r.height/2;"
        "  var dist=Math.sqrt(ox*ox+oy*oy);"
        "  var R=r.width/2*0.62;"
        "  if(dist>R){ox=ox/dist*R;oy=oy/dist*R;}"
        "  joyX=Math.round(ox/R*100);"
        "  joyY=-Math.round(oy/R*100);" /* invert Y: up=positive drive */
        "  moveKnob(ox/R,oy/R);"
        "  sendDrive(joyX,joyY);"
        "}"
        "jOuter.addEventListener('mousedown',function(e){"
        "  joyActive=true;joyUpdate(e.clientX,e.clientY);e.preventDefault();"
        "});"
        "document.addEventListener('mousemove',function(e){"
        "  if(joyActive)joyUpdate(e.clientX,e.clientY);"
        "});"
        "document.addEventListener('mouseup',function(){"
        "  if(joyActive){joyActive=false;moveKnob(0,0);eStop();}"
        "});"
        "jOuter.addEventListener('touchstart',function(e){"
        "  joyActive=true;var t=e.touches[0];joyUpdate(t.clientX,t.clientY);e.preventDefault();"
        "},{passive:false});"
        "document.addEventListener('touchmove',function(e){"
        "  if(joyActive){var t=e.touches[0];joyUpdate(t.clientX,t.clientY);e.preventDefault();}"  /* BUG FIX: closing } was missing */
        "},{passive:false});"
        "document.addEventListener('touchend',function(){"
        "  if(joyActive){joyActive=false;moveKnob(0,0);eStop();}"
        "});"

        /* ── Keyboard ── */
        "function kDown(k){"
        "  if(keys[k])return;keys[k]=true;"
        "  updateKeyDrive();"
        "  if(k==='w'||k==='ArrowUp')document.getElementById('d-up').classList.add('active');"
        "  if(k==='s'||k==='ArrowDown')document.getElementById('d-down').classList.add('active');"
        "  if(k==='a'||k==='ArrowLeft')document.getElementById('d-left').classList.add('active');"
        "  if(k==='d'||k==='ArrowRight')document.getElementById('d-right').classList.add('active');"
        "}"
        "function kUp(k){"
        "  delete keys[k];"
        "  updateKeyDrive();"
        "  document.getElementById('d-up').classList.remove('active');"
        "  document.getElementById('d-down').classList.remove('active');"
        "  document.getElementById('d-left').classList.remove('active');"
        "  document.getElementById('d-right').classList.remove('active');"
        "}"
        "function updateKeyDrive(){"
        "  if(joyActive)return;"
        "  var s=0,d=0;"
        "  if(keys['a']||keys['ArrowLeft'])s=-100;"
        "  if(keys['d']||keys['ArrowRight'])s=100;"
        "  if(keys['w']||keys['ArrowUp'])d=100;"
        "  if(keys['s']||keys['ArrowDown'])d=-100;"
        "  sendDrive(s,d);"
        "}"
        "document.addEventListener('keydown',function(e){"
        "  var k=e.key.toLowerCase();"
        "  if(['w','a','s','d','arrowup','arrowdown','arrowleft','arrowright',' '].indexOf(k)>=0)e.preventDefault();"
        "  if(k===' ')eStop();"
        "  else kDown(k);"
        "});"
        "document.addEventListener('keyup',function(e){kUp(e.key.toLowerCase());});"
        "window.addEventListener('blur',function(){keys={};eStop();});"

        /* -- Gamepad polling -- */
        "window.addEventListener('gamepadconnected',function(){"
        "  if(!gpTimer)gpTimer=setInterval(pollGamepad,50);"
        "});"
        "window.addEventListener('gamepaddisconnected',function(){"
        "  if(gpTimer){clearInterval(gpTimer);gpTimer=null;}"
        "});"
        "function pollGamepad(){"
        "  var gps=navigator.getGamepads?navigator.getGamepads():[];"
        "  var gp=gps[0];if(!gp)return;"
        "  var s=Math.round(gp.axes[0]*100);"  /* left stick X */
        "  var d=-Math.round(gp.axes[1]*100);" /* left stick Y inverted */
        "  if(Math.abs(s)<8)s=0;"              /* dead-zone */
        "  if(Math.abs(d)<8)d=0;"
        "  if(gp.buttons[0]&&gp.buttons[0].pressed)eStop();"
        "  else sendDrive(s,d);"
        "}"

        /* -- Clock -- */
        "var tOff=0;"
        "function syncT(){"
        "  fetch('/time').then(function(r){return r.json();}).then(function(d){"
        "    if(d.synced)tOff=(d.epoch*1000)-Date.now();"
        "    else{fetch('/sync_time?epoch='+Math.floor(Date.now()/1000));tOff=0;}"
        "  }).catch(function(){});"
        "}"
        "syncT();setInterval(syncT,60000);"
        "setInterval(function(){"
        "  var d=new Date(Date.now()+tOff);"
        "  var f=('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)+':'+('0'+d.getSeconds()).slice(-2);"
        "  document.getElementById('clock').textContent=f;"
        "},500);"

        /* -- OTA -- */
        "function openOTA(){document.getElementById('ota-drawer').style.display='flex';}"
        "function closeOTA(){document.getElementById('ota-drawer').style.display='none';}"
        "function doOTA(){"
        "  var f=document.getElementById('ota-file').files[0];"
        "  var msg=document.getElementById('ota-msg');"
        "  var bar=document.getElementById('ota-bar');"
        "  if(!f){msg.textContent='Pick a .bin file first';return;}"
        "  msg.style.color='#a0a0d0';msg.textContent='Uploading...';"
        "  var xhr=new XMLHttpRequest();"
        "  xhr.open('POST','/ota',true);"
        "  xhr.upload.onprogress=function(e){"
        "    if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);bar.style.width=p+'%';msg.textContent='Uploading... '+p+'%';}"
        "  };"
        "  xhr.onload=function(){"
        "    bar.style.width='100%';"
        "    if(xhr.status===200){msg.style.color='#00e5a0';msg.textContent='OTA OK! Rebooting...';setTimeout(function(){location.reload();},10000);}"
        "    else{msg.style.color='#f7736a';msg.textContent='OTA failed: HTTP '+xhr.status;}"
        "  };"
        "  xhr.send(f);"
        "}"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ----------------------------------------------------------------------------
//  Custom URI matcher (strips query string before wildcard match)
//  Required so /stream?t=12345 matches the "/stream" URI handler
// ----------------------------------------------------------------------------
static bool custom_uri_match_fn(const char *reference_uri, const char *uri_to_match, size_t match_upto) {
    char path_only[128];
    if (match_upto >= sizeof(path_only)) return false;
    strncpy(path_only, uri_to_match, match_upto);
    path_only[match_upto] = '\0';
    return httpd_uri_match_wildcard(reference_uri, path_only, match_upto);
}
// ----------------------------------------------------------------------------
//  Server startup
// ----------------------------------------------------------------------------
void webserver_start(void) {
    if (s_server != NULL) {
        ESP_LOGW("WEB", "Already running");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable   = true;
    config.server_port        = WEB_SERVER_PORT;
    config.max_uri_handlers   = 30;
    config.recv_wait_timeout  = 300;   // 300 s — allows OTA + long stream sessions
    config.send_wait_timeout  = 30;    // 30 s (was 10 — too short for OTA uploads)
    config.stack_size         = 8192;
    config.uri_match_fn       = custom_uri_match_fn;  // strips query-string before match

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE("WEB", "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t uris[] = {
        { "/",           HTTP_GET,  root_get_handler,   NULL },
        { "/stream",     HTTP_GET,  stream_handler,     NULL },
        { "/cam_on",     HTTP_GET,  cam_on_handler,     NULL },
        { "/cam_off",    HTTP_GET,  cam_off_handler,    NULL },
        { "/cam_status", HTTP_GET,  cam_status_handler, NULL },
        { "/snapshot",   HTTP_GET,  snapshot_handler,   NULL },
        { "/drive",      HTTP_GET,  drive_handler,      NULL },
        { "/time",       HTTP_GET,  time_handler,       NULL },
        { "/status",     HTTP_GET,  status_handler,     NULL },
        { "/schedule",   HTTP_GET,  schedule_handler,   NULL },
        { "/sync_time",  HTTP_GET,  sync_time_handler,  NULL },
        { "/l1on",       HTTP_GET,  quick_action_handler, NULL },
        { "/l1off",      HTTP_GET,  quick_action_handler, NULL },
        { "/toggle",     HTTP_GET,  quick_action_handler, NULL },
        { "/flash",      HTTP_GET,  quick_action_handler, NULL },
        { "/flash_on",   HTTP_GET,  quick_action_handler, NULL },
        { "/flash_off",  HTTP_GET,  quick_action_handler, NULL },
        { "/diag",        HTTP_GET,  diag_handler,       NULL },
        { "/diag_stream", HTTP_GET,  stream_handler,     NULL },
        { "/ota",        HTTP_POST, ota_post_handler,   NULL },
        { "/ota",        HTTP_OPTIONS, cors_options_handler, NULL },
    };


    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI("WEB", "HTTP server on port %d — http://%s.local", config.server_port, MDNS_HOSTNAME);
}