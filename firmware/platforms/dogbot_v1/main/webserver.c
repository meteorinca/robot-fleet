#include "webserver.h"
#include "config.h"
#include "wifi_mgr.h"
// WS2812 boards define WS2812_NUM_LEDS; simple LED boards define LED_GPIO
#ifdef WS2812_NUM_LEDS
#include "ws2812.h"
#else
#include "led.h"
#endif

#ifdef DISP_MOSI_GPIO
#include "dog_peripherals.h"
#endif
#include "servo.h"
#include "timekeep.h"
#include "dog_actions.h"
#include "ota_mgr.h"

#include <string.h>
#include <stdio.h>
#include "esp_system.h"
// rf.h is only present when board has an RF module
#ifdef RF_RX_GPIO
#include "rf.h"
#endif
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/socket.h>   // send(), close(), MSG_DONTWAIT
#include "esp_wifi.h"     // esp_wifi_scan_*

static httpd_handle_t s_server = NULL;

// ══════════════════════════════════════════════════════════════
//  SSE (Server-Sent Events) — push TTS text to browser
// ══════════════════════════════════════════════════════════════
#define SSE_MAX_CLIENTS 4
static int         s_sse_fds[SSE_MAX_CLIENTS];
static SemaphoreHandle_t s_sse_mutex = NULL;

static void sse_init(void) {
    s_sse_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) s_sse_fds[i] = -1;
}

// Push a line of text to all connected SSE clients.
// Format: "data: <text>\n\n"
void sse_broadcast_tts(const char *text) {
    if (!s_sse_mutex || !text) return;
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "data: %s\n\n", text);
    if (len <= 0) return;
    xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_sse_fds[i] >= 0) {
            int sent = send(s_sse_fds[i], buf, len, MSG_DONTWAIT);
            if (sent < 0) {
                // Client disconnected — remove slot
                close(s_sse_fds[i]);
                s_sse_fds[i] = -1;
                ESP_LOGI("SSE", "Client slot %d removed", i);
            }
        }
    }
    xSemaphoreGive(s_sse_mutex);
}

static esp_err_t sse_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    // Send initial comment so the browser confirms connection
    httpd_resp_send_chunk(req, ": connected\n\n", HTTPD_RESP_USE_STRLEN);

    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_sse_fds[i] < 0) {
            s_sse_fds[i] = fd;
            ESP_LOGI("SSE", "Client registered on slot %d (fd=%d)", i, fd);
            break;
        }
    }
    xSemaphoreGive(s_sse_mutex);

    // Return immediately — do NOT block the httpd worker thread.
    // The socket stays open because we never send the final chunk.
    // sse_broadcast_tts() pushes data directly via send() on the raw fd.
    return ESP_OK;
}

// ══════════════════════════════════════════════════════════════
//  Named-action dispatcher (used by web handlers + scheduler)
// ══════════════════════════════════════════════════════════════
void execute_named_action(const char *action) {
    if      (strcmp(action, "s1on")   == 0) servo_quick_action(1, POS1_ON,  POS1_NEUTRAL);
    else if (strcmp(action, "s1off")  == 0) servo_quick_action(1, POS1_OFF, POS1_NEUTRAL);
    else if (strcmp(action, "s2on")   == 0) servo_quick_action(2, POS2_ON,  POS2_NEUTRAL);
    else if (strcmp(action, "s2off")  == 0) servo_quick_action(2, POS2_OFF, POS2_NEUTRAL);
#if SERVO_COUNT >= 3
    else if (strcmp(action, "s3on")   == 0) servo_quick_action(3, POS3_ON,  POS3_NEUTRAL);
    else if (strcmp(action, "s3off")  == 0) servo_quick_action(3, POS3_OFF, POS3_NEUTRAL);
#endif
#if SERVO_COUNT >= 4
    else if (strcmp(action, "s4on")   == 0) servo_quick_action(4, POS4_ON,  POS4_NEUTRAL);
    else if (strcmp(action, "s4off")  == 0) servo_quick_action(4, POS4_OFF, POS4_NEUTRAL);
#endif
    else if (strcmp(action, "l1on")   == 0) led_action_set(true);
    else if (strcmp(action, "l1off")  == 0) led_action_set(false);
    else if (strcmp(action, "toggle") == 0) led_action_toggle();
    else if (strcmp(action, "hi")     == 0) servo_quick_action(1, 40, POS1_NEUTRAL);
    else if (strcmp(action, "bark")   == 0) dog_audio_play_bark();
    else if (strcmp(action, "paulbot")== 0) dog_audio_play_paulbot();
    else if (strcmp(action, "huh")    == 0) dog_audio_play_named("huh");
    else if (strcmp(action, "yes")    == 0) dog_audio_play_named("yes");
    else if (strcmp(action, "jump")   == 0) dog_audio_play_named("jump");
    else if (strcmp(action, "ding")   == 0) dog_audio_play_named("ding");
    else if (strcmp(action, "random") == 0) dog_audio_play_named("random");
    else if (strcmp(action, "lay")    == 0 || strcmp(action, "lie") == 0) dog_action_send("lay");
    else if (strcmp(action, "stand")  == 0) dog_action_send("stand");
    else if (strcmp(action, "walk_fwd") == 0) dog_action_send("forward");
    else if (strcmp(action, "walk_bwd") == 0) dog_action_send("backward");
    else if (strcmp(action, "bow") == 0) dog_action_send("bow");
    else if (strcmp(action, "lean") == 0) dog_action_send("lean");
    else if (strcmp(action, "wiggle") == 0) dog_action_send("wiggle");
    else if (strcmp(action, "rock") == 0) dog_action_send("rock");
    else if (strcmp(action, "sway") == 0) dog_action_send("sway");
    else if (strcmp(action, "shake") == 0) dog_action_send("shake");
    else if (strcmp(action, "poke") == 0) dog_action_send("poke");
    else if (strcmp(action, "kick") == 0) dog_action_send("kick");
    else if (strcmp(action, "jumpfwd") == 0 || strcmp(action, "jump_fwd") == 0) dog_action_send("jump_fwd");
    else if (strcmp(action, "jumpbck") == 0 || strcmp(action, "jump_bwd") == 0) dog_action_send("jump_bwd");
    // tts:<text> — push text to SSE clients for browser-side synthesis
    else if (strncmp(action, "tts:", 4) == 0) sse_broadcast_tts(action + 4);
    else ESP_LOGW("ACTION", "Unknown action: %s", action);
}

// ══════════════════════════════════════════════════════════════
//  Handlers
// ══════════════════════════════════════════════════════════════

static esp_err_t cors_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── /dog POST handler — same JSON protocol as ESP-Hi example /control ────────
// Body: {"action":"4"}  or  {"move":"F"}
static esp_err_t dog_handler(httpd_req_t *req) {
    char body[64] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[got] = '\0';

    char key[16] = {0}, val[16] = {0};
    // Parse {"key":"val"}
    sscanf(body, "{\"%[^\"]\":\"%[^\"]\"}", key, val);

    bool ok = false;
    if (strcmp(key, "move") == 0 || strcmp(key, "action") == 0) {
        ok = dog_action_send(val);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? "{\"code\":200}" : "{\"error\":\"unknown\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req) {
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, len);
    return ESP_OK;
}

// ══════════════════════════════════════════════════════════════
//  Calibration endpoints
//  The Calibrate tab JS does:
//    GET /start_calibration  → returns {"fl":0,"fr":0,"bl":0,"br":0}
//    POST /adjust            → {"servo":"fl","value":3}
//    GET /exit_calibration   → returns {"code":200}
// ══════════════════════════════════════════════════════════════
static struct { int fl, fr, bl, br; } s_cal = {0};
static bool s_cal_mode = false;

static void cal_apply(void) {
    servo_set_angle(1, 90 + s_cal.fl);   // FL
    servo_set_angle(2, 90 - s_cal.fr);   // FR (mirrored)
    servo_set_angle(3, 90 - s_cal.bl);   // BL (mirrored)
    servo_set_angle(4, 90 + s_cal.br);   // BR
}

static esp_err_t start_calibration_get_handler(httpd_req_t *req) {
    s_cal_mode = true;
    cal_apply();
    char resp[64];
    int len = snprintf(resp, sizeof(resp),
        "{\"fl\":%d,\"fr\":%d,\"bl\":%d,\"br\":%d}",
        s_cal.fl, s_cal.fr, s_cal.bl, s_cal.br);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t exit_calibration_get_handler(httpd_req_t *req) {
    s_cal_mode = false;
    // Return servos to plain neutral
    servo_set_angle(1, 90); servo_set_angle(2, 90);
    servo_set_angle(3, 90); servo_set_angle(4, 90);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t adjust_handler(httpd_req_t *req) {
    if (!s_cal_mode) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not in calibration mode");
        return ESP_FAIL;
    }
    char body[64] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got > 0) {
        char leg[8] = {0};
        int val = 0;
        sscanf(body, "{\"servo\":\"%[^\"]\",\"value\":%d}", leg, &val);
        if      (strcmp(leg, "fl") == 0) s_cal.fl = val;
        else if (strcmp(leg, "fr") == 0) s_cal.fr = val;
        else if (strcmp(leg, "bl") == 0) s_cal.bl = val;
        else if (strcmp(leg, "br") == 0) s_cal.br = val;
        cal_apply();
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t led_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "state", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "toggle") == 0) led_action_toggle();
            else if (strcmp(param, "on") == 0)  led_action_set(true);
            else if (strcmp(param, "off") == 0) led_action_set(false);
        }
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t servo_handler(httpd_req_t *req) {
    char buf[100];
    int servo = 0, angle = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(buf, "num", p, sizeof(p)) == ESP_OK)   servo = atoi(p);
        if (httpd_query_key_value(buf, "angle", p, sizeof(p)) == ESP_OK) angle = atoi(p);
    }
    if (servo >= 1 && servo <= servo_count()) servo_action_set(servo, angle);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

#ifdef RF_RX_GPIO
static esp_err_t send_rf_handler(httpd_req_t *req) {
    char buf[100];
    uint32_t code = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[32];
        if (httpd_query_key_value(buf, "code", p, sizeof(p)) == ESP_OK)
            code = strtoul(p, NULL, 16);
    }
    if (code) {
        rf_send_code(code, 24);
        ESP_LOGI("WEB", "Sent RF 0x%06lX", (unsigned long)code);
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif // RF_RX_GPIO

static esp_err_t time_handler(httpd_req_t *req) {
    // Avoid cJSON heap alloc/free on every poll — plain snprintf instead
    char buf[32];
    timekeep_format(buf, sizeof(buf));
    char resp[96];
    int len = snprintf(resp, sizeof(resp),
        "{\"formatted\":\"%s\",\"epoch\":%lld,\"synced\":%s}",
        buf, (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"status\":\"running\",\"version\":\"%s\",\"epoch\":%lld,\"time_synced\":%s}",
        FW_VERSION, (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

// Quick-action: now async — returns immediately, servo worker does the move
static esp_err_t quick_action_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    // Strip leading '/' and dispatch
    if (uri[0] == '/') uri++;
    execute_named_action(uri);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// /s{N}_{angle} — set servo N to exact angle (holds, no return-to-neutral)
// Example: /s1_90  /s2_0  /s3_180
static esp_err_t servo_angle_uri_handler(httpd_req_t *req) {
    const char *uri = req->uri; // e.g. "/s1_90"
    int servo = 0, angle = 0;
    if (sscanf(uri, "/s%d_%d", &servo, &angle) == 2) {
        if (servo >= 1 && servo <= servo_count()) {
            servo_action_set(servo, angle);
            ESP_LOGI("WEB", "API: servo %d -> %d deg", servo, angle);
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t tts_api_handler(httpd_req_t *req) {
    char text[256] = {0};
    const char *uri = req->uri;

    // Check for path-style: /sendtts:...
    if (strncmp(uri, "/sendtts:", 9) == 0) {
        strncpy(text, uri + 9, sizeof(text) - 1);
    } else {
        // Query-string style: /tts?say=...
        char buf[300];
        if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
            httpd_query_key_value(buf, "say", text, sizeof(text));
        }
    }

    // URL-decode '+' as space (minimal decoder for simple phrases)
    for (char *p = text; *p; p++) if (*p == '+') *p = ' ';

    if (text[0]) {
        ESP_LOGI("WEB", "TTS API: \"%s\"", text);
        sse_broadcast_tts(text);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, text[0] ? "OK" : "Missing ?say=", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t eye_mood_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "val", param, sizeof(param)) == ESP_OK) {
            dog_set_eye_mood(atoi(param));
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t oled_text_handler(httpd_req_t *req) {
    char buf[200];
    char msg[32] = {0};
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        httpd_query_key_value(buf, "msg", msg, sizeof(msg));
    }
    for (char *p = msg; *p; p++) if (*p == '+') *p = ' '; // simple URL decode
    if (msg[0]) {
        dog_set_oled_text(msg, 3000); // show for 3 seconds
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
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

// ── GitHub SPEAKTHISDOG.txt polling task ─────────────────────────────────────
// Gate behind ENABLE_GITHUB_TTS (defined in board_config.h).
// When disabled: saves ~8KB flash (esp_http_client) + 8KB task stack RAM.
#ifdef ENABLE_GITHUB_TTS
#include "esp_http_client.h"

#define GITHUB_TTS_URL "https://raw.githubusercontent.com/meteorinca/mojDogv1/refs/heads/main/SPEAKTHISDOG.txt"

static char s_last_spoken[256] = {0}; // avoid re-speaking same text

static void github_tts_task(void *arg) {
    // Wait for time sync before starting
    for (int i = 0; i < 60 && !timekeep_is_synced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI("GH_TTS", "Starting GitHub TTS polling (every 15 min)");

    while (1) {
        // Sleep until the next 15-minute mark (:00, :15, :30, :45)
        time_t now = timekeep_now();
        struct tm ti;
        localtime_r(&now, &ti);
        int min_in_quarter = ti.tm_min % 15;
        int secs_to_next   = (14 - min_in_quarter) * 60 + (60 - ti.tm_sec);
        if (secs_to_next > 900) secs_to_next = 900;
        ESP_LOGI("GH_TTS", "Next fetch in %d s", secs_to_next);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)secs_to_next * 1000));

        // Fetch the raw text
        char buf[256] = {0};
        esp_http_client_config_t cfg = {
            .url            = GITHUB_TTS_URL,
            .timeout_ms     = 8000,
            .skip_cert_common_name_check = true, // fleet devices may lack CA bundle
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) { vTaskDelay(pdMS_TO_TICKS(60000)); continue; }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            int content_len = esp_http_client_fetch_headers(client);
            if (content_len > 0 && content_len < (int)sizeof(buf) - 1) {
                esp_http_client_read(client, buf, content_len);
                buf[content_len] = '\0';
            } else if (content_len < 0) {
                // chunked — read up to buf size
                int r = esp_http_client_read(client, buf, sizeof(buf) - 1);
                if (r > 0) buf[r] = '\0';
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        // Trim trailing whitespace / newlines
        int blen = strlen(buf);
        while (blen > 0 && (buf[blen-1] == '\n' || buf[blen-1] == '\r' || buf[blen-1] == ' ')) {
            buf[--blen] = '\0';
        }

        if (blen > 0 && strcmp(buf, s_last_spoken) != 0) {
            ESP_LOGI("GH_TTS", "Speaking: \"%s\"", buf);
            strncpy(s_last_spoken, buf, sizeof(s_last_spoken) - 1);
            sse_broadcast_tts(buf);
        } else {
            ESP_LOGI("GH_TTS", "Text unchanged, skipping speak");
        }
    }
}
#endif // ENABLE_GITHUB_TTS

// /schedule — queue an action at an exact wall-clock time or relative delay.
//
// Params (all GET query string):
//   action=<name>   required  e.g. wiggle, hi, stand, s1_90, s2_45, tts:hello
//   at=<epoch>      optional  Unix seconds (integer). Use this for fleet sync.
//   ms=<0-999>      optional  sub-second offset in ms (used with at= for precision)
//   delay=<sec>     optional  schedule N seconds from now
//   delay_ms=<ms>   optional  schedule N milliseconds from now (combines with delay)
//
// Returns JSON: {"ok":true,"action":"...","at":<epoch>,"ms":<ms>}
//
// JupyterLab one-liner (fleet sync):
//   import requests, time
//   t = int(time.time()) + 10   # 10 s from now
//   for n in range(1, 4):
//       requests.get(f"http://paulbot{n}.local:81/schedule?action=wiggle&at={t}")
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

    // Resolve the target time
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

#ifdef DISP_MOSI_GPIO
static esp_err_t audio_post_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char buf[1024];
    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE("WEB", "Audio recv fail");
            return ESP_FAIL;
        }
        dog_audio_play_chunk((const uint8_t *)buf, received);
        remaining -= received;
    }

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif

// ══════════════════════════════════════════════════════════════
//  OTA firmware update handler  POST /ota
//
//  Client sends the raw .bin file as the request body.
//  Use from shell / Jupyter:
//    import requests
//    with open('mojDogv1.bin','rb') as f:
//        requests.post('http://paulbot2.local:81/ota', data=f,
//                      headers={'Content-Type':'application/octet-stream'})
//
//  Or from the web UI (added below in root_get_handler HTML).
// ══════════════════════════════════════════════════════════════
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

    static char buf[OTA_BUF_SIZE];   // static: avoids 4KB stack hit per request
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
        // Log progress every ~64KB
        if (((total - remaining) % 65536) < OTA_BUF_SIZE) {
            ESP_LOGI("OTA", "Progress: %d / %d bytes", total - remaining, total);
        }
    }

    if (ota_end(&h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    // Send success response before restarting so the client sees it
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ota\":\"ok\",\"restart\":true}", HTTPD_RESP_USE_STRLEN);

    // Brief delay so TCP ACK reaches the client
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK; // unreachable
}

// ══════════════════════════════════════════════════════════════
//  WiFi provisioning endpoints
//    GET  /wifi       — list saved networks + AP mode status
//    POST /wifi       — save {ssid, pass} and reboot
//    GET  /wifi_scan  — scan for visible SSIDs
//    DELETE /wifi?delete=N — delete saved credential N
// ══════════════════════════════════════════════════════════════

// GET /wifi — returns {"ap_mode":bool, "saved":["ssid1","ssid2",...]}
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

static bool parse_json_string(const char *json, const char *key, char *out_val, size_t max_len) {
    char key_buf[64];
    snprintf(key_buf, sizeof(key_buf), "\"%s\"", key);
    const char *k = strstr(json, key_buf);
    if (!k) return false;

    // Move past the key name
    k += strlen(key_buf);

    // Find the colon ':' separating key and value
    const char *colon = strchr(k, ':');
    if (!colon) return false;

    // Find the opening quote '"' of the string value
    const char *start = strchr(colon, '"');
    if (!start) return false;
    start++; // skip the opening quote

    // Find the closing quote '"' of the string value
    const char *end = strchr(start, '"');
    if (!end) return false;

    size_t len = end - start;
    if (len >= max_len) len = max_len - 1;
    memcpy(out_val, start, len);
    out_val[len] = '\0';
    return true;
}

// POST /wifi — body: {"ssid":"...", "pass":"..."}
static esp_err_t wifi_post_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    char body[200] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        httpd_resp_send(req, "{\"error\":\"No body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    body[got] = '\0';

    // Simple JSON parse: {"ssid":"...","pass":"..."}
    char ssid[33] = {0}, pass[65] = {0};
    parse_json_string(body, "ssid", ssid, sizeof(ssid));
    parse_json_string(body, "pass", pass, sizeof(pass));

    if (!ssid[0]) {
        httpd_resp_send(req, "{\"error\":\"Missing SSID\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = wifi_save_credential(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send(req, "{\"error\":\"NVS write failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI("WEB", "WiFi credential saved: %s — rebooting in 3s", ssid);

    // Reboot after a brief delay so the HTTP response gets sent
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK; // unreachable
}

// DELETE /wifi?delete=N — remove saved credential at index N
static esp_err_t wifi_delete_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    char qs[64];
    int idx = -1;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char p[8];
        if (httpd_query_key_value(qs, "delete", p, sizeof(p)) == ESP_OK) {
            idx = atoi(p);
        }
    }

    if (idx < 0) {
        httpd_resp_send(req, "{\"error\":\"Missing ?delete=N\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = wifi_nvs_credential_delete(idx);
    if (err != ESP_OK) {
        httpd_resp_send(req, "{\"error\":\"Delete failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /wifi_scan — scan for visible networks, returns {"networks":[{"ssid":"...","rssi":-50},...]}
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    // Start a blocking scan
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 100, .max = 300 } },
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_send(req, "{\"networks\":[],\"error\":\"scan failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;  // cap results

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        httpd_resp_send(req, "{\"networks\":[],\"error\":\"OOM\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    // Build JSON response
    char resp[1024];
    int pos = snprintf(resp, sizeof(resp), "{\"networks\":[");
    int added = 0;
    for (int i = 0; i < ap_count && pos < (int)sizeof(resp) - 100; i++) {
        if (ap_records[i].ssid[0] == '\0') continue;  // skip hidden
        // Deduplicate by SSID
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char*)ap_records[i].ssid, (char*)ap_records[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        if (added > 0) pos += snprintf(resp + pos, sizeof(resp) - pos, ",");
        pos += snprintf(resp + pos, sizeof(resp) - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d}",
                        (char*)ap_records[i].ssid, ap_records[i].rssi);
        added++;
    }
    pos += snprintf(resp + pos, sizeof(resp) - pos, "]}");
    free(ap_records);

    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

// ══════════════════════════════════════════════════════════════
//  Server startup
// ══════════════════════════════════════════════════════════════
void webserver_start(void) {
    if (s_server != NULL) {
        ESP_LOGW("WEB", "Already running");
        return;
    }

    sse_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.server_port      = WEB_SERVER_PORT;
    config.max_uri_handlers = 80;     // increased for WiFi provisioning + OTA + new sounds
    config.recv_wait_timeout  = 300;  // 300 s — allows large OTA binary uploads
    config.send_wait_timeout  = 10;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard; // enables /s1_* /sendtts:* patterns

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE("WEB", "Failed to start HTTP server");
        return;
    }

    // Table-driven registration
    static const httpd_uri_t uris[] = {
        { "/",          HTTP_GET,  root_get_handler,       NULL },
        // Dog animation control (POST, same protocol as ESP-Hi example)
        { "/dog",       HTTP_POST, dog_handler,            NULL },
        { "/dog",       HTTP_OPTIONS, cors_options_handler,NULL },
        // Calibration endpoints (reuse example protocol)
        { "/start_calibration", HTTP_GET,  start_calibration_get_handler, NULL },
        { "/exit_calibration",  HTTP_GET,  exit_calibration_get_handler,  NULL },
        { "/adjust",            HTTP_POST, adjust_handler,                NULL },
        { "/led",       HTTP_GET,  led_handler,            NULL },
        { "/servo",     HTTP_GET,  servo_handler,          NULL },
#ifdef RF_RX_GPIO
        { "/send",      HTTP_GET,  send_rf_handler,        NULL },
#endif
        { "/time",      HTTP_GET,  time_handler,           NULL },
        { "/status",    HTTP_GET,  status_handler,         NULL },
        { "/schedule",  HTTP_GET,  schedule_handler,       NULL },
        // SSE push channel
        { "/events",    HTTP_GET,  sse_handler,            NULL },
        { "/sync_time", HTTP_GET,  sync_time_handler,      NULL },
        // OLED APIs
        { "/eye_mood",  HTTP_GET,  eye_mood_handler,       NULL },
        { "/oled_text", HTTP_GET,  oled_text_handler,      NULL },
        // Servo direct-angle shortcuts: /s1_90  /s2_0  etc.
        { "/s1_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
        { "/s2_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
#if SERVO_COUNT >= 3
        { "/s3_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
#endif
#if SERVO_COUNT >= 4
        { "/s4_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
#endif
        // Named quick-actions
        { "/s1on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s1off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/s2on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s2off",     HTTP_GET,  quick_action_handler,   NULL },
#if SERVO_COUNT >= 3
        { "/s3on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s3off",     HTTP_GET,  quick_action_handler,   NULL },
#endif
#if SERVO_COUNT >= 4
        { "/s4on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s4off",     HTTP_GET,  quick_action_handler,   NULL },
#endif
        { "/l1on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/l1off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/toggle",    HTTP_GET,  quick_action_handler,   NULL },
        { "/hi",        HTTP_GET,  quick_action_handler,   NULL },
        { "/bark",      HTTP_GET,  quick_action_handler,   NULL },
        { "/paulbot",   HTTP_GET,  quick_action_handler,   NULL },
        { "/lay",       HTTP_GET,  quick_action_handler,   NULL },
        { "/lie",       HTTP_GET,  quick_action_handler,   NULL },
        { "/stand",     HTTP_GET,  quick_action_handler,   NULL },
        { "/walk_fwd",  HTTP_GET,  quick_action_handler,   NULL },
        { "/walk_bwd",  HTTP_GET,  quick_action_handler,   NULL },
        { "/bow",       HTTP_GET,  quick_action_handler,   NULL },
        { "/lean",      HTTP_GET,  quick_action_handler,   NULL },
        { "/wiggle",    HTTP_GET,  quick_action_handler,   NULL },
        { "/rock",      HTTP_GET,  quick_action_handler,   NULL },
        { "/sway",      HTTP_GET,  quick_action_handler,   NULL },
        { "/shake",     HTTP_GET,  quick_action_handler,   NULL },
        { "/poke",      HTTP_GET,  quick_action_handler,   NULL },
        { "/kick",      HTTP_GET,  quick_action_handler,   NULL },
        { "/jumpfwd",   HTTP_GET,  quick_action_handler,   NULL },
        { "/jumpbck",   HTTP_GET,  quick_action_handler,   NULL },
        { "/jump_fwd",  HTTP_GET,  quick_action_handler,   NULL },
        { "/jump_bwd",  HTTP_GET,  quick_action_handler,   NULL },
        { "/huh",       HTTP_GET,  quick_action_handler,   NULL },
        { "/yes",       HTTP_GET,  quick_action_handler,   NULL },
        { "/jump",      HTTP_GET,  quick_action_handler,   NULL },
        { "/ding",      HTTP_GET,  quick_action_handler,   NULL },
        { "/random",    HTTP_GET,  quick_action_handler,   NULL },
#ifdef DISP_MOSI_GPIO
        { "/audio",     HTTP_POST, audio_post_handler,     NULL },
        { "/audio",     HTTP_OPTIONS, cors_options_handler,NULL },
#endif
        // OTA firmware update — POST the raw .bin file body
        { "/ota",       HTTP_POST, ota_post_handler,       NULL },
        { "/ota",       HTTP_OPTIONS, cors_options_handler,NULL },
        // WiFi provisioning endpoints
        { "/wifi",      HTTP_GET,    wifi_get_handler,     NULL },
        { "/wifi",      HTTP_POST,   wifi_post_handler,    NULL },
        { "/wifi",      HTTP_DELETE, wifi_delete_handler,   NULL },
        { "/wifi",      HTTP_OPTIONS,cors_options_handler,  NULL },
        { "/wifi_scan", HTTP_GET,    wifi_scan_handler,     NULL },

    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    // /sendtts:... — wildcard path, registered separately
    static const httpd_uri_t sendtts_uri = {
        .uri      = "/sendtts:*",
        .method   = HTTP_GET,
        .handler  = tts_api_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &sendtts_uri);

#ifdef ENABLE_GITHUB_TTS
    // GitHub SPEAKTHISDOG.txt polling task (8KB stack — uses esp_http_client)
    xTaskCreate(github_tts_task, "gh_tts", 8192, NULL, 2, NULL);
#endif

    ESP_LOGI("WEB", "HTTP server on port %d", config.server_port);
}
