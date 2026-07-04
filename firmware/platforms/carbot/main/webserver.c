// main/webserver.c  —  CarBot
// ============================================================================
//  HTTP web server for CarBot.
//  Key endpoints:
//    GET  /              — serve index.html (embedded)
//    POST /drive         — {throttle:-100..100, steer:0..100}
//    GET  /drive?t=N&s=N — quick throttle+steer GET command
//    GET  /steer?pos=N   — set steering position 0-100 (50=center)
//    GET  /steer_center  — re-zero the steering estimate
//    GET  /throttle?v=N  — set drive speed (-100 to +100)
//    GET  /brake         — active brake
//    GET  /coast         — coast / free-wheel
//    GET  /motor_state   — JSON status of motors
//    GET  /us_data       — ultrasonic distance JSON
//    POST /dog           — {action:"...", move:"..."} (kept for compatibility)
//    GET  /status        — firmware status JSON
//    GET  /time          — NTP time JSON
//    GET  /schedule      — schedule an action
//    GET  /oled?text=..  — display text on OLED
//    GET  /show_ip       — display IP on OLED
//    GET  /tone?f=&d=    — buzzer tone
//    GET  /demo?type=    — buzzer demo
//    POST /ota           — OTA firmware update
//    GET  /wifi          — WiFi credentials list
//    POST /wifi          — save credentials + reboot
//    DEL  /wifi?delete=N — delete saved credential
//    GET  /wifi_scan     — scan for networks
// ============================================================================
#include "webserver.h"
#include "config.h"
#include "led.h"
#include "buzzer.h"
#include "timekeep.h"
#include "ota_mgr.h"
#include "motor.h"
#include "ultrasonic.h"
#include "oled.h"
#include <sys/param.h>
#include <string.h>
#include <stdio.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/socket.h>
#include <errno.h>

static httpd_handle_t s_server = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ══════════════════════════════════════════════════════════════
//  Named-action dispatcher (used by web handlers + scheduler)
// ══════════════════════════════════════════════════════════════
void execute_named_action(const char *action) {
    ESP_LOGI("ACTION", "Executing: %s", action);

    if      (strcmp(action, "brake")       == 0) motor_drive_brake();
    else if (strcmp(action, "coast")       == 0) motor_drive_coast();
    else if (strcmp(action, "steer_center")== 0) motor_steer_reset_center();
    else if (strcmp(action, "l1on")        == 0) led_action_set(true);
    else if (strcmp(action, "l1off")       == 0) led_action_set(false);
    else if (strcmp(action, "toggle")      == 0) led_action_toggle();
    else if (strcmp(action, "us_on")       == 0) { ultrasonic_set_active(true);  oled_set_mode(OLED_MODE_ULTRASONIC_VIEW); }
    else if (strcmp(action, "us_off")      == 0) { ultrasonic_set_active(false); oled_set_mode(OLED_MODE_NORMAL); }
    else if (strcmp(action, "show_ip")     == 0) oled_set_mode(OLED_MODE_SHOW_IP);
    else ESP_LOGW("ACTION", "Action ignored on carbot: %s", action);
}

// ══════════════════════════════════════════════════════════════
//  CORS pre-flight
// ══════════════════════════════════════════════════════════════
static esp_err_t cors_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS, DELETE");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── Root ──────────────────────────────────────────────────────────────────────
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

// ── Motor state JSON ─────────────────────────────────────────────────────────
static esp_err_t motor_state_handler(httpd_req_t *req) {
    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"throttle\":%d,\"steer\":%d,\"braking\":%s}",
        motor_drive_get_throttle(),
        motor_steer_get_pos(),
        motor_is_braking() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

// ── POST /drive  — {throttle:-100..100, steer:0..100} ─────────────────────────
static esp_err_t drive_post_handler(httpd_req_t *req) {
    char body[128] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[got] = '\0';

    // Parse simple JSON: {"throttle":N,"steer":N}
    int throttle = 0, steer = 50;
    char *tp = strstr(body, "\"throttle\"");
    char *sp = strstr(body, "\"steer\"");
    if (tp) {
        tp = strchr(tp, ':');
        if (tp) throttle = atoi(tp + 1);
    }
    if (sp) {
        sp = strchr(sp, ':');
        if (sp) steer = atoi(sp + 1);
    }

    // Clamp
    if (throttle < -100) throttle = -100;
    if (throttle >  100) throttle =  100;
    if (steer < 0)   steer = 0;
    if (steer > 100) steer = 100;

    motor_set((int8_t)throttle, (uint8_t)steer);
    ESP_LOGI("WEB", "POST /drive throttle=%d steer=%d", throttle, steer);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── GET /drive?t=N&s=N ────────────────────────────────────────────────────────
static esp_err_t drive_get_handler(httpd_req_t *req) {
    char buf[64];
    int throttle = 0, steer = 50;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(buf, "t", p, sizeof(p)) == ESP_OK) throttle = atoi(p);
        if (httpd_query_key_value(buf, "s", p, sizeof(p)) == ESP_OK) steer    = atoi(p);
    }
    if (throttle < -100) throttle = -100;
    if (throttle >  100) throttle =  100;
    if (steer < 0)   steer = 0;
    if (steer > 100) steer = 100;

    motor_set((int8_t)throttle, (uint8_t)steer);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── GET /steer?pos=N ─────────────────────────────────────────────────────────
static esp_err_t steer_handler(httpd_req_t *req) {
    char buf[32];
    int pos = 50;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(buf, "pos", p, sizeof(p)) == ESP_OK) pos = atoi(p);
    }
    if (pos < 0)   pos = 0;
    if (pos > 100) pos = 100;
    motor_steer_set((uint8_t)pos);
    ESP_LOGI("WEB", "GET /steer pos=%d", pos);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── GET /steer_center ─────────────────────────────────────────────────────────
static esp_err_t steer_center_handler(httpd_req_t *req) {
    motor_steer_reset_center();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── GET /throttle?v=N ────────────────────────────────────────────────────────
static esp_err_t throttle_handler(httpd_req_t *req) {
    char buf[32];
    int v = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(buf, "v", p, sizeof(p)) == ESP_OK) v = atoi(p);
    }
    if (v < -100) v = -100;
    if (v >  100) v =  100;
    if (v > 0)       motor_drive_forward((uint8_t)v);
    else if (v < 0)  motor_drive_backward((uint8_t)(-v));
    else             motor_drive_brake();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── GET /brake ───────────────────────────────────────────────────────────────
static esp_err_t brake_handler(httpd_req_t *req) {
    motor_drive_brake();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── GET /coast ───────────────────────────────────────────────────────────────
static esp_err_t coast_handler(httpd_req_t *req) {
    motor_drive_coast();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── Ultrasonic ───────────────────────────────────────────────────────────────
static esp_err_t us_data_handler(httpd_req_t *req) {
    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"active\":%s,\"dist\":%.1f}",
        ultrasonic_is_active() ? "true" : "false",
        ultrasonic_get_distance());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

// ── /dog POST  — legacy action protocol ──────────────────────────────────────
static esp_err_t dog_handler(httpd_req_t *req) {
    char body[128] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[got] = '\0';

    char key[16] = {0}, val[16] = {0};
    if (sscanf(body, "{\"%[^\"]\":\"%[^\"]\"}", key, val) == 2) {
        if (strcmp(key, "action") == 0 || strcmp(key, "move") == 0) {
            execute_named_action(val);
        }
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── Quick-action GET handler ─────────────────────────────────────────────────
static esp_err_t quick_action_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    if (uri[0] == '/') uri++;
    execute_named_action(uri);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── Time ─────────────────────────────────────────────────────────────────────
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

// ── Status ────────────────────────────────────────────────────────────────────
static esp_err_t status_handler(httpd_req_t *req) {
    char resp[256];
    int len = snprintf(resp, sizeof(resp),
        "{\"status\":\"running\",\"version\":\"%s\",\"epoch\":%lld,"
        "\"time_synced\":%s,\"throttle\":%d,\"steer\":%d,\"braking\":%s}",
        FW_VERSION, (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false",
        motor_drive_get_throttle(),
        motor_steer_get_pos(),
        motor_is_braking() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

// ── Sync time ─────────────────────────────────────────────────────────────────
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

// ── OLED text ─────────────────────────────────────────────────────────────────
static esp_err_t oled_text_handler(httpd_req_t *req) {
    char text[64] = {0};
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "text", text, sizeof(text)) == ESP_OK) {
            for (char *p = text; *p; p++) if (*p == '+') *p = ' ';
            char decoded[64] = {0};
            int d = 0;
            for (int i = 0; text[i] && d < 63; i++) {
                if (text[i] == '%' && text[i+1] && text[i+2]) {
                    char hex[3] = {text[i+1], text[i+2], 0};
                    decoded[d++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    decoded[d++] = text[i];
                }
            }
            oled_set_text(decoded, 4000);
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── Show IP ───────────────────────────────────────────────────────────────────
static esp_err_t show_ip_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_SHOW_IP);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── Ultrasonic activate/deactivate ────────────────────────────────────────────
static esp_err_t us_on_handler(httpd_req_t *req) {
    ultrasonic_set_active(true);
    oled_set_mode(OLED_MODE_ULTRASONIC_VIEW);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static esp_err_t us_off_handler(httpd_req_t *req) {
    ultrasonic_set_active(false);
    oled_set_mode(OLED_MODE_NORMAL);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── /schedule ─────────────────────────────────────────────────────────────────
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
        if (httpd_query_key_value(qs, "action",   p, sizeof(p)) == ESP_OK) strncpy(action, p, sizeof(action) - 1);
        if (httpd_query_key_value(qs, "at",       p, sizeof(p)) == ESP_OK) at = (time_t)strtol(p, NULL, 10);
        if (httpd_query_key_value(qs, "ms",       p, sizeof(p)) == ESP_OK) extra_ms = atoi(p);
        if (httpd_query_key_value(qs, "delay",    p, sizeof(p)) == ESP_OK) delay_sec = atoi(p);
        if (httpd_query_key_value(qs, "delay_ms", p, sizeof(p)) == ESP_OK) delay_ms  = atoi(p);
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

// ── Buzzer ────────────────────────────────────────────────────────────────────
static esp_err_t buzzer_tone_handler(httpd_req_t *req) {
    char f_str[32] = {0};
    uint32_t f = 1000, d = 100;
    if (httpd_req_get_url_query_str(req, f_str, sizeof(f_str)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(f_str, "f", val, sizeof(val)) == ESP_OK) f = atoi(val);
        if (httpd_query_key_value(f_str, "d", val, sizeof(val)) == ESP_OK) d = atoi(val);
    }
    buzzer_play_tone(f, d);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t buzzer_demo_handler(httpd_req_t *req) {
    char q_str[32] = {0};
    if (httpd_req_get_url_query_str(req, q_str, sizeof(q_str)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(q_str, "type", val, sizeof(val)) == ESP_OK) {
            if      (strcmp(val, "coin")     == 0) buzzer_demo_coin();
            else if (strcmp(val, "gameover") == 0) buzzer_demo_gameover();
            else if (strcmp(val, "siren")    == 0) buzzer_demo_siren();
            else if (strcmp(val, "laser")    == 0) buzzer_demo_laser();
            else if (strcmp(val, "mario")    == 0) buzzer_demo_mario();
            else if (strcmp(val, "1up")      == 0) buzzer_demo_1up();
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ── OTA ───────────────────────────────────────────────────────────────────────
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

// ── WiFi provisioning ─────────────────────────────────────────────────────────
#include "wifi_mgr.h"

static bool parse_json_string(const char *json, const char *key, char *out_val, size_t max_len) {
    char key_buf[64];
    snprintf(key_buf, sizeof(key_buf), "\"%s\"", key);
    const char *k = strstr(json, key_buf);
    if (!k) return false;
    k += strlen(key_buf);
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    const char *start = strchr(colon, '"');
    if (!start) return false;
    start++;
    const char *end = strchr(start, '"');
    if (!end) return false;
    size_t len = end - start;
    if (len >= max_len) len = max_len - 1;
    memcpy(out_val, start, len);
    out_val[len] = '\0';
    return true;
}

static esp_err_t wifi_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    char resp[512];
    int pos = 0;
    pos += snprintf(resp + pos, sizeof(resp) - pos,
                    "{\"ap_mode\":%s,\"saved\":[",
                    wifi_is_ap_mode() ? "true" : "false");
    int count = wifi_nvs_credential_count();
    for (int i = 0; i < count; i++) {
        char ssid[33] = {0}, pass[65] = {0};
        if (wifi_nvs_credential_get(i, ssid, sizeof(ssid), pass, sizeof(pass))) {
            if (i > 0) pos += snprintf(resp + pos, sizeof(resp) - pos, ",");
            pos += snprintf(resp + pos, sizeof(resp) - pos, "\"%s\"", ssid);
        }
    }
    pos += snprintf(resp + pos, sizeof(resp) - pos, "]}");
    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    char body[200] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) { httpd_resp_send(req, "{\"error\":\"No body\"}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    body[got] = '\0';
    char ssid[33] = {0}, pass[65] = {0};
    parse_json_string(body, "ssid", ssid, sizeof(ssid));
    parse_json_string(body, "pass", pass, sizeof(pass));
    if (!ssid[0]) { httpd_resp_send(req, "{\"error\":\"Missing SSID\"}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    esp_err_t err = wifi_save_credential(ssid, pass);
    if (err != ESP_OK) { httpd_resp_send(req, "{\"error\":\"NVS write failed\"}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI("WEB", "WiFi credential saved: %s — rebooting in 3s", ssid);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t wifi_delete_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    char qs[64]; int idx = -1;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char p[8];
        if (httpd_query_key_value(qs, "delete", p, sizeof(p)) == ESP_OK) idx = atoi(p);
    }
    if (idx < 0) { httpd_resp_send(req, "{\"error\":\"Missing ?delete=N\"}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    esp_err_t err = wifi_nvs_credential_delete(idx);
    if (err != ESP_OK) { httpd_resp_send(req, "{\"error\":\"Delete failed\"}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 100, .max = 300 } },
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) { httpd_resp_send(req, "{\"networks\":[],\"error\":\"scan failed\"}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;
    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) { httpd_resp_send(req, "{\"networks\":[],\"error\":\"OOM\"}", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    char resp[1024];
    int pos = snprintf(resp, sizeof(resp), "{\"networks\":[");
    int added = 0;
    for (int i = 0; i < ap_count && pos < (int)sizeof(resp) - 100; i++) {
        if (ap_records[i].ssid[0] == '\0') continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char*)ap_records[i].ssid, (char*)ap_records[j].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (added > 0) pos += snprintf(resp + pos, sizeof(resp) - pos, ",");
        pos += snprintf(resp + pos, sizeof(resp) - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d}", (char*)ap_records[i].ssid, ap_records[i].rssi);
        added++;
    }
    pos += snprintf(resp + pos, sizeof(resp) - pos, "]}");
    free(ap_records);
    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

// ── Custom URI matcher ────────────────────────────────────────────────────────
static bool custom_uri_match_fn(const char *reference_uri, const char *uri_to_match, size_t match_upto) {
    char path_only[128];
    if (match_upto >= sizeof(path_only)) return false;
    strncpy(path_only, uri_to_match, match_upto);
    path_only[match_upto] = '\0';
    return httpd_uri_match_wildcard(reference_uri, path_only, match_upto);
}

// ══════════════════════════════════════════════════════════════
//  Server startup
// ══════════════════════════════════════════════════════════════
void webserver_start(void) {
    if (s_server != NULL) { ESP_LOGW("WEB", "Already running"); return; }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable   = true;
    config.server_port        = WEB_SERVER_PORT;
    config.max_uri_handlers   = 60;
    config.recv_wait_timeout  = 300;
    config.send_wait_timeout  = 10;
    config.stack_size         = 8192;
    config.uri_match_fn       = custom_uri_match_fn;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE("WEB", "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t uris[] = {
        { "/",              HTTP_GET,    root_get_handler,      NULL },
        // ── Motor control ─────────────────────────────────────────────────────
        { "/drive",         HTTP_POST,   drive_post_handler,    NULL },
        { "/drive",         HTTP_OPTIONS,cors_options_handler,  NULL },
        { "/drive",         HTTP_GET,    drive_get_handler,     NULL },
        { "/steer",         HTTP_GET,    steer_handler,         NULL },
        { "/steer_center",  HTTP_GET,    steer_center_handler,  NULL },
        { "/throttle",      HTTP_GET,    throttle_handler,      NULL },
        { "/brake",         HTTP_GET,    brake_handler,         NULL },
        { "/coast",         HTTP_GET,    coast_handler,         NULL },
        { "/motor_state",   HTTP_GET,    motor_state_handler,   NULL },
        // ── Sensors ───────────────────────────────────────────────────────────
        { "/us_data",       HTTP_GET,    us_data_handler,       NULL },
        { "/us_on",         HTTP_GET,    us_on_handler,         NULL },
        { "/us_off",        HTTP_GET,    us_off_handler,        NULL },
        // ── Legacy action protocol ────────────────────────────────────────────
        { "/dog",           HTTP_POST,   dog_handler,           NULL },
        { "/dog",           HTTP_OPTIONS,cors_options_handler,  NULL },
        // ── Status / time ─────────────────────────────────────────────────────
        { "/status",        HTTP_GET,    status_handler,        NULL },
        { "/time",          HTTP_GET,    time_handler,          NULL },
        { "/sync_time",     HTTP_GET,    sync_time_handler,     NULL },
        { "/schedule",      HTTP_GET,    schedule_handler,      NULL },
        // ── OLED ──────────────────────────────────────────────────────────────
        { "/oled",          HTTP_GET,    oled_text_handler,     NULL },
        { "/show_ip",       HTTP_GET,    show_ip_handler,       NULL },
        // ── LED quick actions ─────────────────────────────────────────────────
        { "/l1on",          HTTP_GET,    quick_action_handler,  NULL },
        { "/l1off",         HTTP_GET,    quick_action_handler,  NULL },
        { "/toggle",        HTTP_GET,    quick_action_handler,  NULL },
        // ── Buzzer ────────────────────────────────────────────────────────────
        { "/tone",          HTTP_GET,    buzzer_tone_handler,   NULL },
        { "/demo",          HTTP_GET,    buzzer_demo_handler,   NULL },
        // ── OTA ───────────────────────────────────────────────────────────────
        { "/ota",           HTTP_POST,   ota_post_handler,      NULL },
        { "/ota",           HTTP_OPTIONS,cors_options_handler,  NULL },
        // ── WiFi ──────────────────────────────────────────────────────────────
        { "/wifi",          HTTP_GET,    wifi_get_handler,      NULL },
        { "/wifi",          HTTP_POST,   wifi_post_handler,     NULL },
        { "/wifi",          HTTP_DELETE, wifi_delete_handler,   NULL },
        { "/wifi",          HTTP_OPTIONS,cors_options_handler,  NULL },
        { "/wifi_scan",     HTTP_GET,    wifi_scan_handler,     NULL },
    };

    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI("WEB", "CarBot HTTP server on port %d", config.server_port);
}
