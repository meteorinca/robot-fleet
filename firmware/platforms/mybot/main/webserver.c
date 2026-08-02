#include "webserver.h"
#include "config.h"
#include "led.h"
#include "buzzer.h"
#include "timekeep.h"
#include "ota_mgr.h"
#include "servo.h"
#include "ultrasonic.h"
#include "oled.h"
#include <sys/param.h>
#include <string.h>
#include <stdio.h>
#include "esp_system.h"
#include "esp_wifi.h"
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
#include <errno.h>

static httpd_handle_t s_server = NULL;

// ══════════════════════════════════════════════════════════════
//  SSE (Server-Sent Events) — push TTS text to browser
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

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

// ══════════════════════════════════════════════════════════════
//  Named-action dispatcher (used by web handlers + scheduler)
// ══════════════════════════════════════════════════════════════
void execute_named_action(const char *action) {
    ESP_LOGI("ACTION", "Executing: %s", action);
    // Blink for any servo-related action (s1..., s2...)
    if (strncmp(action, "s1", 2) == 0 || strncmp(action, "s2", 2) == 0) {
        led_blink(3, 80);
    }

    if      (strcmp(action, "s1on")   == 0) servo_action_set(1, servo_get_last_angle(1));
    else if (strcmp(action, "s1off")  == 0) servo_detach(1);
    else if (strcmp(action, "us_on")  == 0) { ultrasonic_set_active(true); oled_set_mode(OLED_MODE_ULTRASONIC_VIEW); }
    else if (strcmp(action, "us_off") == 0) { ultrasonic_set_active(false); oled_set_mode(OLED_MODE_NORMAL); }
    else if (strcmp(action, "l1on")   == 0) led_action_set(true);
    else if (strcmp(action, "l1off")  == 0) led_action_set(false);
    else if (strcmp(action, "toggle") == 0) led_action_toggle();
    else if (strcmp(action, "grnon")  == 0) led_grn_set(true);
    else if (strcmp(action, "grnoff") == 0) led_grn_set(false);
    else if (strcmp(action, "grntog") == 0) led_grn_toggle();
    else if (strcmp(action, "redon")  == 0) led_red_set(true);
    else if (strcmp(action, "redoff") == 0) led_red_set(false);
    else if (strcmp(action, "redtog") == 0) led_red_toggle();
    else if (strcmp(action, "hi")     == 0) servo_quick_action(1, 40, POS1_NEUTRAL);
    else ESP_LOGW("ACTION", "Action ignored on mybot: %s", action);
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
    // Parse {"key":"val"} or {"action":"val"}
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

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    oled_set_mode(OLED_MODE_FIREWORKS);
    return ESP_OK;
}

// Servo endpoints
static esp_err_t servo_handler(httpd_req_t *req) {
    char buf[100];
    int servo = 0, angle = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(buf, "num", p, sizeof(p)) == ESP_OK)   servo = atoi(p);
        if (httpd_query_key_value(buf, "angle", p, sizeof(p)) == ESP_OK) angle = atoi(p);
    }
    if (servo >= 1 && servo <= servo_count()) {
        led_blink(3, 100);
        servo_action_set(servo, angle);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t servo_angle_uri_handler(httpd_req_t *req) {
    const char *uri = req->uri; // e.g. "/s1_90"
    int servo = 0, angle = 0;
    if (sscanf(uri, "/s%d_%d", &servo, &angle) == 2) {
        if (servo >= 1 && servo <= servo_count()) {
            led_blink(3, 100);
            servo_action_set(servo, angle);
            ESP_LOGI("WEB", "API: servo %d -> %d deg", servo, angle);
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t random_look_handler(httpd_req_t *req) {
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(buf, "on", val, sizeof(val)) == ESP_OK) {
            bool enable = (atoi(val) != 0);
            servo_set_random_look(enable);
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

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
    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"status\":\"running\",\"version\":\"%s\",\"epoch\":%lld,\"time_synced\":%s}",
        FW_VERSION, (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static void send_cmd_html_response(httpd_req_t *req) {
    static const char html[] =
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;text-align:center;padding:50px 20px;background:#0a0a0a;color:#fff;margin:0;}"
        "h1{color:#ffd700;font-size:24px;margin-bottom:12px;}"
        "p{color:#aaa;font-size:16px;margin-bottom:28px;}"
        ".btn{display:inline-block;padding:16px 32px;background:#ffd700;color:#000;text-decoration:none;border-radius:12px;font-weight:bold;font-size:18px;box-shadow:0 4px 14px rgba(255,215,0,0.3);}"
        ".btn:active{transform:scale(0.97);}"
        "</style></head><body>"
        "<h1>MyBot Received Your Command!</h1>"
        "<p>Your robot is executing the command now.</p>"
        "<a href='javascript:history.back()' class='btn'>Back</a>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, html, sizeof(html) - 1);
}

// ── MScript Mission Execution ────────────────────────────────────────────────
#define MAX_MISSION_LEN 512

static TaskHandle_t s_mission_task_handle = NULL;

static void mission_runner_task(void *arg) {
    char *script = (char *)arg;
    ESP_LOGI("MISSION", "Starting MScript: %s", script);

    char *saveptr = NULL;
    char *token = strtok_r(script, "|", &saveptr);
    while (token != NULL) {
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }

        if (token[0] != '\0') {
            if (strncmp(token, "delay:", 6) == 0 || strncmp(token, "wait:", 5) == 0) {
                const char *val_str = (token[0] == 'd') ? token + 6 : token + 5;
                int ms = atoi(val_str);
                if (ms > 0) {
                    if (ms > 10000) ms = 10000;
                    vTaskDelay(pdMS_TO_TICKS(ms));
                }
            } else if (strncmp(token, "servo:", 6) == 0) {
                int num = 1, angle = 90;
                if (sscanf(token + 6, "%d:%d", &num, &angle) == 2) {
                    if (num >= 1 && num <= servo_count()) {
                        servo_action_set(num, angle);
                    }
                }
            } else if (strncmp(token, "oled:", 5) == 0 || strncmp(token, "msg:", 4) == 0) {
                const char *msg = (token[0] == 'o') ? token + 5 : token + 4;
                oled_set_text(msg, 4000);
            } else {
                execute_named_action(token);
            }
        }
        token = strtok_r(NULL, "|", &saveptr);
    }

    ESP_LOGI("MISSION", "MScript finished");
    free(script);
    s_mission_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t mission_api_handler(httpd_req_t *req) {
    char buf[MAX_MISSION_LEN];
    char mscript[MAX_MISSION_LEN] = {0};

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "m", mscript, sizeof(mscript)) != ESP_OK) {
            httpd_query_key_value(buf, "mission", mscript, sizeof(mscript));
        }
    }

    char decoded[MAX_MISSION_LEN] = {0};
    int d = 0;
    for (int i = 0; mscript[i] && d < (MAX_MISSION_LEN - 1); i++) {
        if (mscript[i] == '+') {
            decoded[d++] = ' ';
        } else if (mscript[i] == '%' && mscript[i+1] && mscript[i+2]) {
            char hex[3] = { mscript[i+1], mscript[i+2], 0 };
            decoded[d++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            decoded[d++] = mscript[i];
        }
    }

    if (decoded[0] != '\0') {
        if (s_mission_task_handle != NULL) {
            vTaskDelete(s_mission_task_handle);
            s_mission_task_handle = NULL;
        }

        char *script_copy = strdup(decoded);
        if (script_copy != NULL) {
            BaseType_t ret = xTaskCreate(mission_runner_task, "mscript_run", 3072, script_copy, 3, &s_mission_task_handle);
            if (ret != pdPASS) {
                free(script_copy);
                s_mission_task_handle = NULL;
            }
        }
    }

    send_cmd_html_response(req);
    return ESP_OK;
}

// Quick-action: now async — returns immediately
static esp_err_t quick_action_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    ESP_LOGI("WEB", "Quick action URI: %s", uri);
    // Strip leading '/' and dispatch
    if (uri[0] == '/') uri++;
    execute_named_action(uri);
    send_cmd_html_response(req);
    return ESP_OK;
}

// Servo endpoints removed for mybot.

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
        // Note: SSE broadcast was removed. If TTS is needed in future, implement via polling/websocket.
    }
    send_cmd_html_response(req);
    return ESP_OK;
}

static esp_err_t oled_text_handler(httpd_req_t *req) {
    char text[64] = {0};
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "text", text, sizeof(text)) == ESP_OK) {
            for (char *p = text; *p; p++) if (*p == '+') *p = ' ';
            // naive hex decode for %20 etc
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
            oled_set_text(decoded, 4000); // show for 4 seconds
        }
    }
    send_cmd_html_response(req);
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

extern bool g_btn1_state;
extern bool g_btn2_state;
extern bool g_led_direct_mode;

static esp_err_t led_direct_handler(httpd_req_t *req) {
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(buf, "on", val, sizeof(val)) == ESP_OK) {
            g_led_direct_mode = (atoi(val) != 0);
            if (!g_led_direct_mode) {
                led_grn_set(false);
                led_red_set(false);
            }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t btn_data_handler(httpd_req_t *req) {
    char resp[128];
    int len = snprintf(resp, sizeof(resp), "{\"btn1\":%s,\"btn2\":%s,\"game_mode\":%d}", 
        g_btn1_state ? "true" : "false",
        g_btn2_state ? "true" : "false",
        (int)oled_get_mode());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t game_on_h_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_PONG_H);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t game_on_v_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_PONG_V);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t game_flappy_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_FLAPPY);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t game_dino_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_DINO);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t game_snake_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_SNAKE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t game_pacman_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_PACMAN); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_frogger_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_FROGGER); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_racing_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_RACING); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_math_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_MATH); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_3d_handler(httpd_req_t *req) {
    char buf[32];
    extern int g_override_anim_idx;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "idx", param, sizeof(param)) == ESP_OK) {
            g_override_anim_idx = atoi(param);
        }
    }
    oled_set_mode(OLED_MODE_3D_SHOWCASE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static esp_err_t game_truth_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_TRUTH_TABLE); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_piano_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_BUZZER_PIANO); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_us_shooter_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_US_SHOOTER); ultrasonic_set_active(true); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
// New fun animation endpoints
static esp_err_t anim_mario_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_MARIO_DANCE); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_fireworks_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_FIREWORKS); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_matrix_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_MATRIX_RAIN); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_invader_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_SPACE_INVADER); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_heartbeat_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_HEARTBEAT); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }

static esp_err_t big_yawn_handler(httpd_req_t *req) {
    // Non-blocking: just sets the OLED mode. The oled_eyes_task drives the animation.
    oled_set_mode(OLED_MODE_BIG_YAWN);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t game_off_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_NORMAL);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t show_ip_handler(httpd_req_t *req) {
    oled_set_mode(OLED_MODE_SHOW_IP);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

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
//       requests.get(f"http://dogbot{n}.local:81/schedule?action=wiggle&at={t}")
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

//  Buzzer endpoints
// GET /tone?f=1000&d=100
static esp_err_t buzzer_tone_handler(httpd_req_t *req) {
    char f_str[16] = {0};
    uint32_t f = 1000;
    uint32_t d = 100;
    if (httpd_req_get_url_query_str(req, f_str, sizeof(f_str)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(f_str, "f", val, sizeof(val)) == ESP_OK) f = atoi(val);
        if (httpd_query_key_value(f_str, "d", val, sizeof(val)) == ESP_OK) d = atoi(val);
    }
    buzzer_play_tone(f, d);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// GET /demo?type=coin
static esp_err_t buzzer_demo_handler(httpd_req_t *req) {
    char q_str[32] = {0};
    if (httpd_req_get_url_query_str(req, q_str, sizeof(q_str)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(q_str, "type", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "coin") == 0) buzzer_demo_coin();
            else if (strcmp(val, "gameover") == 0) buzzer_demo_gameover();
            else if (strcmp(val, "siren") == 0) buzzer_demo_siren();
            else if (strcmp(val, "laser") == 0) buzzer_demo_laser();
            else if (strcmp(val, "mario") == 0) oled_set_mode(OLED_MODE_MARIO_DANCE);
            else if (strcmp(val, "1up") == 0) buzzer_demo_1up();
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}
// ══════════════════════════════════════════════════════════════
//  OTA firmware update handler  POST /ota
//
//  Client sends the raw .bin file as the request body.
//  Use from shell / Jupyter:
//    import requests
//    with open('mojDogv1.bin','rb') as f:
//        requests.post('http://dogbot2.local:81/ota', data=f,
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

#include "wifi_mgr.h"

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

// POST /wifi — body: {"ssid":"...","pass":"..."}
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

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

// DELETE /wifi?delete=N
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

// GET /wifi_scan
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

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
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        httpd_resp_send(req, "{\"networks\":[],\"error\":\"OOM\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    char resp[1024];
    int pos = snprintf(resp, sizeof(resp), "{\"networks\":[");
    int added = 0;
    for (int i = 0; i < ap_count && pos < (int)sizeof(resp) - 100; i++) {
        if (ap_records[i].ssid[0] == '\0') continue;
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
//  Custom URI Matcher (to handle query strings with wildcards)
// ══════════════════════════════════════════════════════════════
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
    if (s_server != NULL) {
        ESP_LOGW("WEB", "Already running");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.server_port      = WEB_SERVER_PORT;
    config.max_uri_handlers = 100;     // increased for new endpoints
    config.recv_wait_timeout  = 300;  // 300 s — allows large OTA binary uploads
    config.send_wait_timeout  = 10;
    config.stack_size = 8192;
    config.uri_match_fn = custom_uri_match_fn; // handles query strings correctly with wildcards

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE("WEB", "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t uris[] = {
        { "/s1on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s1off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/s2on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s2off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/",          HTTP_GET,  root_get_handler,       NULL },
        { "/dog",       HTTP_POST, dog_handler,            NULL },
        { "/dog",       HTTP_OPTIONS, cors_options_handler,NULL },
        { "/time",      HTTP_GET,  time_handler,           NULL },
        { "/status",    HTTP_GET,  status_handler,         NULL },
        { "/schedule",  HTTP_GET,  schedule_handler,       NULL },
        { "/api/mission", HTTP_GET, mission_api_handler,    NULL },
        { "/mission",     HTTP_GET, mission_api_handler,    NULL },
        { "/us_data",   HTTP_GET,  us_data_handler,        NULL },
        { "/sync_time", HTTP_GET,  sync_time_handler,      NULL },
        { "/led_direct",HTTP_GET,  led_direct_handler,     NULL },
        { "/btn_data",  HTTP_GET,  btn_data_handler,       NULL },
        { "/game_on_h", HTTP_GET,  game_on_h_handler,      NULL },
        { "/game_on_v", HTTP_GET,  game_on_v_handler,      NULL },
        { "/game_flappy",HTTP_GET, game_flappy_handler,    NULL },
        { "/game_dino", HTTP_GET,  game_dino_handler,      NULL },
        { "/game_snake",HTTP_GET,  game_snake_handler,     NULL },
        { "/game_pacman",HTTP_GET, game_pacman_handler,    NULL },
        { "/game_frogger",HTTP_GET,game_frogger_handler,   NULL },
        { "/game_racing",HTTP_GET, game_racing_handler,    NULL },
        { "/game_math",  HTTP_GET, game_math_handler,      NULL },
        { "/game_truth", HTTP_GET, game_truth_handler,     NULL },
        { "/game_us_shooter", HTTP_GET, game_us_shooter_handler, NULL },
        { "/game_piano", HTTP_GET, game_piano_handler,     NULL },
        { "/anim_3d",    HTTP_GET, anim_3d_handler,        NULL },
        { "/game_off",  HTTP_GET,  game_off_handler,       NULL },
        { "/show_ip",   HTTP_GET,  show_ip_handler,        NULL },
        // New fun animation endpoints
        { "/anim_mario",    HTTP_GET, anim_mario_handler,    NULL },
        { "/anim_fireworks",HTTP_GET, anim_fireworks_handler,NULL },
        { "/anim_matrix",   HTTP_GET, anim_matrix_handler,   NULL },
        { "/anim_invader",  HTTP_GET, anim_invader_handler,  NULL },
        { "/anim_heartbeat",HTTP_GET, anim_heartbeat_handler,NULL },
        // ... (rest of quick actions)
        { "/l1on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/l1off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/toggle",    HTTP_GET,  quick_action_handler,   NULL },
        { "/grnon",     HTTP_GET,  quick_action_handler,   NULL },
        { "/grnoff",    HTTP_GET,  quick_action_handler,   NULL },
        { "/grntog",    HTTP_GET,  quick_action_handler,   NULL },
        { "/redon",     HTTP_GET,  quick_action_handler,   NULL },
        { "/redoff",    HTTP_GET,  quick_action_handler,   NULL },
        { "/redtog",    HTTP_GET,  quick_action_handler,   NULL },
        { "/hi",        HTTP_GET,  quick_action_handler,   NULL },
        { "/us_on",     HTTP_GET,  quick_action_handler,   NULL },
        { "/us_off",    HTTP_GET,  quick_action_handler,   NULL },
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
        { "/oled",      HTTP_GET,  oled_text_handler,      NULL },

#ifdef DISP_MOSI_GPIO
        { "/audio",     HTTP_POST, audio_post_handler,     NULL },
        { "/audio",     HTTP_OPTIONS, cors_options_handler,NULL },
#endif
        { "/ota",       HTTP_POST, ota_post_handler,       NULL },
        { "/ota",       HTTP_OPTIONS, cors_options_handler,NULL },
        { "/tone",           HTTP_GET, buzzer_tone_handler,    NULL },
        { "/demo",           HTTP_GET, buzzer_demo_handler,    NULL },
        { "/servo",     HTTP_GET,  servo_handler,          NULL },
        { "/s1_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
        { "/s2_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
        { "/random_look", HTTP_GET, random_look_handler,   NULL },
        { "/big_yawn",    HTTP_GET, big_yawn_handler,      NULL },
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
