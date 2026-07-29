#include "webserver.h"
#include "config.h"
#include "led.h"
#include "servo.h"
#include "oled.h"
#include "dog_peripherals.h"
#include "dog_actions.h"
#include "timekeep.h"
#include "ota_mgr.h"
#include "wifi_mgr.h"

#include <sys/param.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef RF_RX_GPIO
#include "rf.h"
#endif

static const char *TAG = "SPEAKER_WEB";
static httpd_handle_t s_server = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

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
                close(s_sse_fds[i]);
                s_sse_fds[i] = -1;
                ESP_LOGI("SSE", "Client slot %d disconnected", i);
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

    return ESP_OK;
}

// ══════════════════════════════════════════════════════════════
//  Named-action dispatcher (used by web handlers + scheduler)
// ══════════════════════════════════════════════════════════════
void execute_named_action(const char *action) {
    ESP_LOGI("ACTION", "Executing: %s", action);
    if (strncmp(action, "s1", 2) == 0) {
        led_blink(3, 80);
    }

    if      (strcmp(action, "s1on")   == 0) servo_action_set(1, servo_get_last_angle(1));
    else if (strcmp(action, "s1off")  == 0) servo_detach(1);
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
    else if (strcmp(action, "bark")   == 0) dog_audio_play_bark();
    else if (strcmp(action, "paulbot")== 0) dog_audio_play_paulbot();
    else if (strcmp(action, "huh")    == 0) dog_audio_play_named("huh");
    else if (strcmp(action, "yes")    == 0) dog_audio_play_named("yes");
    else if (strcmp(action, "jump")   == 0) dog_audio_play_named("jump");
    else if (strcmp(action, "ding")   == 0) dog_audio_play_named("ding");
    else if (strcmp(action, "random") == 0) dog_audio_play_random();
    else if (strncmp(action, "tts:", 4) == 0) sse_broadcast_tts(action + 4);
    else if (strcmp(action, "anim_eyes")      == 0) oled_set_mode(OLED_MODE_NORMAL);
    else if (strcmp(action, "anim_fireworks") == 0) oled_set_mode(OLED_MODE_FIREWORKS);
    else if (strcmp(action, "anim_matrix")    == 0) oled_set_mode(OLED_MODE_MATRIX_RAIN);
    else if (strcmp(action, "anim_heartbeat") == 0) oled_set_mode(OLED_MODE_HEARTBEAT);
    else ESP_LOGW("ACTION", "Unknown or unhandled action: %s", action);
}

// ══════════════════════════════════════════════════════════════
//  HTTP Handlers
// ══════════════════════════════════════════════════════════════

static esp_err_t cors_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
    speaker_play_drum_beat(); // Smooth single drum kick when someone connects
    return ESP_OK;
}

static esp_err_t dog_handler(httpd_req_t *req) {
    char body[64] = {0};
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

static esp_err_t audio_post_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char qs[128] = {0};
    char interrupt_str[16] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        if (httpd_query_key_value(qs, "interrupt", interrupt_str, sizeof(interrupt_str)) == ESP_OK) {
            if (strcmp(interrupt_str, "1") == 0 || strcasecmp(interrupt_str, "true") == 0) {
                dog_audio_stop();
            }
        }
    }
    dog_audio_reset_stop_flag();

    char buf[1024];
    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        if (dog_audio_is_stopped()) {
            ESP_LOGW("WEB", "Audio streaming aborted by stop request");
            break;
        }
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

static esp_err_t stop_audio_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    dog_audio_stop();
    httpd_resp_send(req, "{\"stopped\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

typedef struct {
    char url[256];
} http_stream_args_t;

static void http_audio_stream_task(void *pvParameters) {
    http_stream_args_t *args = (http_stream_args_t *)pvParameters;
    if (!args) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI("WEB_STREAM", "Streaming audio from URL: %s", args->url);

    char host[128] = {0};
    int port = 80;
    char path[256] = {0};

    const char *p = args->url;
    if (strncmp(p, "http://", 7) == 0) p += 7;

    const char *slash = strchr(p, '/');
    if (slash) {
        strncpy(path, slash, sizeof(path) - 1);
        size_t host_len = slash - p;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, p, host_len);
    } else {
        strncpy(host, p, sizeof(host) - 1);
        strcpy(path, "/");
    }

    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE("WEB_STREAM", "DNS lookup failed for %s", host);
        free(args);
        vTaskDelete(NULL);
        return;
    }

    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        ESP_LOGE("WEB_STREAM", "Failed to allocate socket");
        freeaddrinfo(res);
        free(args);
        vTaskDelete(NULL);
        return;
    }

    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE("WEB_STREAM", "Socket connect failed errno=%d", errno);
        close(s);
        freeaddrinfo(res);
        free(args);
        vTaskDelete(NULL);
        return;
    }
    freeaddrinfo(res);

    char req_hdr[512];
    int req_len = snprintf(req_hdr, sizeof(req_hdr),
                           "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                           path, host);
    write(s, req_hdr, req_len);

    char buf[1024];
    int header_done = 0;
    int buf_pos = 0;
    while (!header_done) {
        int r = read(s, buf + buf_pos, 1);
        if (r <= 0) break;
        buf_pos++;
        if (buf_pos >= 4 && memcmp(buf + buf_pos - 4, "\r\n\r\n", 4) == 0) {
            header_done = 1;
        }
        if (buf_pos >= sizeof(buf) - 1) break;
    }

    int r;
    while ((r = read(s, buf, sizeof(buf))) > 0) {
        if (dog_audio_is_stopped()) break;
        dog_audio_play_chunk((const uint8_t *)buf, r);
    }

    close(s);
    free(args);
    ESP_LOGI("WEB_STREAM", "Socket audio stream finished.");
    vTaskDelete(NULL);
}

static esp_err_t play_url_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char qs[300] = {0};
    char url[256] = {0};
    char interrupt_str[16] = {0};

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        httpd_query_key_value(qs, "url", url, sizeof(url));
        httpd_query_key_value(qs, "interrupt", interrupt_str, sizeof(interrupt_str));
    }

    if (!url[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing ?url=\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (strcmp(interrupt_str, "1") == 0 || strcasecmp(interrupt_str, "true") == 0) {
        dog_audio_stop();
    }

    http_stream_args_t *args = malloc(sizeof(http_stream_args_t));
    if (!args) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    strncpy(args->url, url, sizeof(args->url) - 1);

    if (xTaskCreate(http_audio_stream_task, "http_audio_stream", 4096, args, 5, NULL) != pdPASS) {
        free(args);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create fail");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"streaming\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t tts_api_handler(httpd_req_t *req) {
    char text[256] = {0};
    const char *uri = req->uri;

    if (strncmp(uri, "/sendtts:", 9) == 0) {
        strncpy(text, uri + 9, sizeof(text) - 1);
    } else {
        char buf[300];
        if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
            httpd_query_key_value(buf, "say", text, sizeof(text));
        }
    }

    for (char *p = text; *p; p++) if (*p == '+') *p = ' ';

    if (text[0]) {
        ESP_LOGI("WEB", "TTS API: \"%s\"", text);
        sse_broadcast_tts(text);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, text[0] ? "OK" : "Missing ?say=", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t sound_clip_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    char name[32] = {0};
    int repeat = 1;
    bool interrupt = false;

    char qs[128] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(qs, "name", name, sizeof(name)) != ESP_OK) {
            if (uri[0] == '/') sscanf(uri, "/%31[^?]", name);
        }
        if (httpd_query_key_value(qs, "repeat", p, sizeof(p)) == ESP_OK) repeat = atoi(p);
        if (httpd_query_key_value(qs, "interrupt", p, sizeof(p)) == ESP_OK) {
            if (strcmp(p, "1") == 0 || strcasecmp(p, "true") == 0) interrupt = true;
        }
    } else {
        if (uri[0] == '/') strncpy(name, uri + 1, sizeof(name) - 1);
    }

    if (!name[0]) strncpy(name, "bark", sizeof(name) - 1);

    dog_audio_play_named_ex(name, repeat, interrupt);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    int len = snprintf(resp, sizeof(resp), "{\"ok\":true,\"sound\":\"%s\",\"repeat\":%d,\"interrupt\":%s}",
                       name, repeat, interrupt ? "true" : "false");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t eye_mood_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "val", param, sizeof(param)) == ESP_OK) {
            oled_set_emotion((eye_emotion_t)atoi(param));
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t oled_text_handler(httpd_req_t *req) {
    char text[64] = {0};
    char buf[200];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "msg", text, sizeof(text)) != ESP_OK) {
            httpd_query_key_value(buf, "text", text, sizeof(text));
        }
    }
    
    char decoded[64] = {0};
    int d = 0;
    for (int i = 0; text[i] && d < 63; i++) {
        if (text[i] == '+') {
            decoded[d++] = ' ';
        } else if (text[i] == '%' && text[i+1] && text[i+2]) {
            char hex[3] = {text[i+1], text[i+2], 0};
            decoded[d++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            decoded[d++] = text[i];
        }
    }
    if (decoded[0]) {
        oled_set_text(decoded, 4000);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t anim_handler(httpd_req_t *req) {
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[8];
        if (httpd_query_key_value(buf, "mode", p, sizeof(p)) == ESP_OK) {
            oled_set_mode((oled_mode_t)atoi(p));
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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
    if (servo >= 1 && servo <= servo_count()) {
        led_blink(3, 100);
        servo_action_set(servo, angle);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t servo_angle_uri_handler(httpd_req_t *req) {
    const char *uri = req->uri;
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

static esp_err_t quick_action_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    if (uri[0] == '/') uri++;
    execute_named_action(uri);
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
    for (int i = 0; i < ap_count; i++) {
        if (i > 0) pos += snprintf(resp + pos, sizeof(resp) - pos, ",");
        pos += snprintf(resp + pos, sizeof(resp) - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d}",
                        (char *)ap_records[i].ssid, ap_records[i].rssi);
    }
    pos += snprintf(resp + pos, sizeof(resp) - pos, "]}");
    free(ap_records);

    httpd_resp_send(req, resp, pos);
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

static esp_err_t game_on_h_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_PONG_H); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_on_v_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_PONG_V); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_flappy_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_FLAPPY); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_dino_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_DINO); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_snake_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_SNAKE); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_pacman_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_PACMAN); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_frogger_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_FROGGER); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_racing_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_RACING); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_math_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_MATH); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_truth_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_TRUTH_TABLE); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_3d_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_3D_SHOWCASE); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_mario_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_MARIO_DANCE); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_fireworks_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_FIREWORKS); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_matrix_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_MATRIX_RAIN); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_invader_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_SPACE_INVADER); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t anim_heartbeat_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_HEARTBEAT); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t big_yawn_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_BIG_YAWN); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t game_off_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_NORMAL); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }
static esp_err_t show_ip_handler(httpd_req_t *req) { oled_set_mode(OLED_MODE_SHOW_IP); httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK; }

void webserver_start(void) {
    if (s_server != NULL) return;
    sse_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers  = 60;
    config.max_open_sockets  = 7;
    config.backlog_conn      = 5;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.server_port       = WEB_SERVER_PORT;
    config.ctrl_port         = 32768;
    config.lru_purge_enable  = true;

    ESP_LOGI(TAG, "Starting SpeakerBot HTTP server on port %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",                   .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/events",             .method = HTTP_GET,  .handler = sse_handler },
        { .uri = "/dog",                .method = HTTP_POST, .handler = dog_handler },
        { .uri = "/dog",                .method = HTTP_OPTIONS, .handler = cors_options_handler },
        { .uri = "/audio",              .method = HTTP_POST, .handler = audio_post_handler },
        { .uri = "/audio",              .method = HTTP_OPTIONS, .handler = cors_options_handler },
        { .uri = "/stop",               .method = HTTP_GET,  .handler = stop_audio_handler },
        { .uri = "/stop",               .method = HTTP_POST, .handler = stop_audio_handler },
        { .uri = "/stop",               .method = HTTP_OPTIONS, .handler = cors_options_handler },
        { .uri = "/sound",              .method = HTTP_GET,  .handler = sound_clip_handler },
        { .uri = "/sound",              .method = HTTP_OPTIONS, .handler = cors_options_handler },
        { .uri = "/play_url",           .method = HTTP_GET,  .handler = play_url_handler },
        { .uri = "/play_url",           .method = HTTP_POST, .handler = play_url_handler },
        { .uri = "/play_url",           .method = HTTP_OPTIONS, .handler = cors_options_handler },
        { .uri = "/tts",                .method = HTTP_GET,  .handler = tts_api_handler },
        { .uri = "/sendtts:*",          .method = HTTP_GET,  .handler = tts_api_handler },
        
        // Sound clips
        { .uri = "/bark",               .method = HTTP_GET,  .handler = sound_clip_handler },
        { .uri = "/paulbot",            .method = HTTP_GET,  .handler = sound_clip_handler },
        { .uri = "/huh",                .method = HTTP_GET,  .handler = sound_clip_handler },
        { .uri = "/yes",                .method = HTTP_GET,  .handler = sound_clip_handler },
        { .uri = "/jump",               .method = HTTP_GET,  .handler = sound_clip_handler },
        { .uri = "/ding",               .method = HTTP_GET,  .handler = sound_clip_handler },
        { .uri = "/random",             .method = HTTP_GET,  .handler = sound_clip_handler },

        { .uri = "/eye_mood",           .method = HTTP_GET,  .handler = eye_mood_handler },
        { .uri = "/oled_text",          .method = HTTP_GET,  .handler = oled_text_handler },
        { .uri = "/anim",               .method = HTTP_GET,  .handler = anim_handler },
        { .uri = "/schedule",           .method = HTTP_GET,  .handler = schedule_handler },

        // Servo
        { .uri = "/servo",              .method = HTTP_GET,  .handler = servo_handler },
        { .uri = "/s1_*",               .method = HTTP_GET,  .handler = servo_angle_uri_handler },

        // Firmware OTA
        { .uri = "/ota",                .method = HTTP_POST, .handler = ota_post_handler },
        { .uri = "/ota",                .method = HTTP_OPTIONS, .handler = cors_options_handler },

        // WiFi
        { .uri = "/wifi",               .method = HTTP_GET,  .handler = wifi_get_handler },
        { .uri = "/wifi",               .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/wifi",               .method = HTTP_DELETE,.handler = wifi_delete_handler },
        { .uri = "/wifi",               .method = HTTP_OPTIONS,.handler = cors_options_handler },
        { .uri = "/wifi_scan",          .method = HTTP_GET,  .handler = wifi_scan_handler },

        // System
        { .uri = "/time",               .method = HTTP_GET,  .handler = time_handler },
        { .uri = "/status",             .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/sync_time",          .method = HTTP_GET,  .handler = sync_time_handler },

        // Button/LED Direct & Games
        { .uri = "/btn_data",           .method = HTTP_GET,  .handler = btn_data_handler },
        { .uri = "/led_direct",         .method = HTTP_GET,  .handler = led_direct_handler },
        { .uri = "/game_on_h",          .method = HTTP_GET,  .handler = game_on_h_handler },
        { .uri = "/game_on_v",          .method = HTTP_GET,  .handler = game_on_v_handler },
        { .uri = "/game_flappy",        .method = HTTP_GET,  .handler = game_flappy_handler },
        { .uri = "/game_dino",          .method = HTTP_GET,  .handler = game_dino_handler },
        { .uri = "/game_snake",         .method = HTTP_GET,  .handler = game_snake_handler },
        { .uri = "/game_pacman",        .method = HTTP_GET,  .handler = game_pacman_handler },
        { .uri = "/game_frogger",       .method = HTTP_GET,  .handler = game_frogger_handler },
        { .uri = "/game_racing",        .method = HTTP_GET,  .handler = game_racing_handler },
        { .uri = "/game_math",          .method = HTTP_GET,  .handler = game_math_handler },
        { .uri = "/game_truth",         .method = HTTP_GET,  .handler = game_truth_handler },
        { .uri = "/anim_3d",            .method = HTTP_GET,  .handler = anim_3d_handler },
        { .uri = "/anim_mario",         .method = HTTP_GET,  .handler = anim_mario_handler },
        { .uri = "/anim_fireworks",     .method = HTTP_GET,  .handler = anim_fireworks_handler },
        { .uri = "/anim_matrix",        .method = HTTP_GET,  .handler = anim_matrix_handler },
        { .uri = "/anim_invader",       .method = HTTP_GET,  .handler = anim_invader_handler },
        { .uri = "/anim_heartbeat",     .method = HTTP_GET,  .handler = anim_heartbeat_handler },
        { .uri = "/big_yawn",           .method = HTTP_GET,  .handler = big_yawn_handler },
        { .uri = "/game_off",           .method = HTTP_GET,  .handler = game_off_handler },
        { .uri = "/show_ip",            .method = HTTP_GET,  .handler = show_ip_handler },

        // Fallback for quick actions
        { .uri = "/*",                  .method = HTTP_GET,  .handler = quick_action_handler }
    };

    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
}
