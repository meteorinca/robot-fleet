// firmware/common/telemetry.h
// ============================================================================
// Fleet-Wide Unified Telemetry & Capability Specification
//
// Provides standardized, non-blocking, zero-allocation telemetry generation
// across all robot platforms in the fleet.
//
// Endpoints supported with this schema:
//   - GET /status
//   - GET /api/info
//   - GET /info
//
// Schema returned:
//   {
//     "status": "running",
//     "platform": "dogbot_v1" | "simplebot" | "rfbot" | ...,
//     "firmware": "1.0",
//     "version": "1.0",
//     "hostname": "paulbot2",
//     "name": "paulbot2",
//     "device_number": 2,
//     "device": "paulbot2",
//     "uptime": 3600,
//     "free_heap": 182400,
//     "epoch": 1728345678,
//     "time_synced": true
//   }
// ============================================================================
#pragma once

#include <stdio.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "timekeep.h"

// ── Default Fallbacks ────────────────────────────────────────────────────────
#ifndef ROBOT_PLATFORM
#define ROBOT_PLATFORM      "esp32_device"
#endif

#ifndef FW_VERSION
#define FW_VERSION          "1.0"
#endif

#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME       "esp32"
#endif

#ifndef DEVICE_NUMBER
#define DEVICE_NUMBER       1
#endif

/**
 * Builds the fleet-standard unified telemetry JSON string.
 *
 * Fast, non-blocking, zero heap allocation (uses stack buffer).
 * Uptime is derived from the hardware 64-bit microsecond timer (negligible cost).
 */
static inline int telemetry_build_json(char *buf, size_t max_len) {
    return snprintf(buf, max_len,
        "{\"status\":\"running\","
        "\"platform\":\"%s\","
        "\"firmware\":\"%s\","
        "\"version\":\"%s\","
        "\"hostname\":\"%s\","
        "\"name\":\"%s\","
        "\"device_number\":%d,"
        "\"device\":\"%s\","
        "\"uptime\":%lld,"
        "\"free_heap\":%lu,"
        "\"epoch\":%lld,"
        "\"time_synced\":%s}",
        ROBOT_PLATFORM,
        FW_VERSION,
        FW_VERSION,
        MDNS_HOSTNAME,
        MDNS_HOSTNAME,
        DEVICE_NUMBER,
        MDNS_HOSTNAME,
        (long long)(esp_timer_get_time() / 1000000ULL),
        (unsigned long)esp_get_free_heap_size(),
        (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false");
}

/**
 * Builds the fleet-standard telemetry JSON string with additional platform-specific
 * fields appended before the closing JSON brace.
 *
 * Example: extra_fields = "\"streaming\":false,\"throttle\":0"
 */
static inline int telemetry_build_json_with_extra(char *buf, size_t max_len, const char *extra_fields) {
    if (!extra_fields || extra_fields[0] == '\0') {
        return telemetry_build_json(buf, max_len);
    }
    return snprintf(buf, max_len,
        "{\"status\":\"running\","
        "\"platform\":\"%s\","
        "\"firmware\":\"%s\","
        "\"version\":\"%s\","
        "\"hostname\":\"%s\","
        "\"name\":\"%s\","
        "\"device_number\":%d,"
        "\"device\":\"%s\","
        "\"uptime\":%lld,"
        "\"free_heap\":%lu,"
        "\"epoch\":%lld,"
        "\"time_synced\":%s,"
        "%s}",
        ROBOT_PLATFORM,
        FW_VERSION,
        FW_VERSION,
        MDNS_HOSTNAME,
        MDNS_HOSTNAME,
        DEVICE_NUMBER,
        MDNS_HOSTNAME,
        (long long)(esp_timer_get_time() / 1000000ULL),
        (unsigned long)esp_get_free_heap_size(),
        (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false",
        extra_fields);
}

/**
 * Common HTTP GET handler for /status, /api/info, and /info endpoints.
 * Emits the unified telemetry JSON with permissive CORS headers.
 */
static inline esp_err_t telemetry_handler(httpd_req_t *req) {
    char resp[384];
    int len = telemetry_build_json(resp, sizeof(resp));
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}
