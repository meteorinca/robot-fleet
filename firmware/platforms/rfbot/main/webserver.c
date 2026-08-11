#include "webserver.h"
#include "wifi_mgr.h"
#include "esp_wifi.h"
#include "config.h"
#include "led.h"
#include "timekeep.h"
#include "ota_mgr.h"
#include "servo.h"
#include <string.h>
#include <stdio.h>
#include "esp_system.h"
// rf.h and cJSON are only needed when board has an RF module
#ifdef RF_RX_GPIO
#include "rf.h"
#include "rf_relay_config.h"
#include "cJSON.h"
// ── RF Outlet Button Config (edit rf_outlets_config.h to add/remove outlets) ─
typedef struct { const char *label; uint32_t on_code; uint32_t off_code; } rf_outlet_t;
static const rf_outlet_t s_outlets[] = {
#define RF_OUTLET(lbl, on, off) { lbl, (uint32_t)(on), (uint32_t)(off) },
#include "rf_outlets_config.h"
#undef RF_OUTLET
};
#define RF_OUTLET_COUNT ((int)(sizeof(s_outlets)/sizeof(s_outlets[0])))
#endif

#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/socket.h>   // send(), close(), MSG_DONTWAIT

/* Forward declaration — httpd_req_t is now defined via esp_http_server.h above */
#ifdef RF_RX_GPIO
static void rf_outlets_card_send(httpd_req_t *req);
#endif

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
    ESP_LOGI("ACTION", "Executing: %s", action);
    // Blink for any servo-related action (s1..., s2...)
    if (strncmp(action, "s1", 2) == 0 || strncmp(action, "s2", 2) == 0) {
        led_blink(3, 80);
    }

    if      (strcmp(action, "s1on")   == 0) servo_quick_action(1, POS1_ON,  POS1_NEUTRAL);
    else if (strcmp(action, "s1off")  == 0) servo_quick_action(1, POS1_OFF, POS1_NEUTRAL);
    else if (strcmp(action, "s2on")   == 0) servo_quick_action(2, POS2_ON,  POS2_NEUTRAL);
    else if (strcmp(action, "s2off")  == 0) servo_quick_action(2, POS2_OFF, POS2_NEUTRAL);
    else if (strcmp(action, "l1on")   == 0) led_action_set(true);
    else if (strcmp(action, "l1off")  == 0) led_action_set(false);
    else if (strcmp(action, "toggle") == 0) led_action_toggle();
    else if (strcmp(action, "hi")     == 0) servo_quick_action(1, 40, POS1_NEUTRAL);
    // tts:<text> — push text to SSE clients for browser-side synthesis
    else if (strncmp(action, "tts:", 4) == 0) sse_broadcast_tts(action + 4);
    else ESP_LOGW("ACTION", "Action ignored on RFbot: %s", action);
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
    /* ── Part 1: everything up to and including the RF Radio card ─────────── */
    static const char html_pre[] =
        "<!DOCTYPE html>"
        "<html lang='en'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<meta name='apple-mobile-web-app-capable' content='yes'>"
        "<title>RFBot Control</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent;}"
        "html,body{height:100%;overflow-y:auto;width:100%;font-family:system-ui,sans-serif;background:#0d0d1a;color:#e0e0f0;}"
        ".content{padding:20px 10px;display:flex;flex-direction:column;align-items:center;}"
        ".card{background:#11112a;border:1px solid #1e1e3a;border-radius:14px;padding:18px;margin-bottom:14px;width:100%;max-width:460px;}"
        ".card.disabled{opacity:0.4;filter:grayscale(1);pointer-events:none;position:relative;}"
        ".card.disabled::after{content:'NOT SUPPORTED';position:absolute;top:10px;right:12px;font-size:9px;font-weight:900;color:#f7736a;letter-spacing:1px;border:1px solid #f7736a;padding:2px 4px;border-radius:4px;}"
        ".card h2{font-size:13px;font-weight:700;color:#7c6af7;text-transform:uppercase;letter-spacing:1px;margin-bottom:12px;}"
        ".time-big{font-size:36px;font-weight:700;font-family:monospace;color:#00e5a0;text-align:center;}"
        ".epoch{font-size:12px;color:#444;margin-top:4px;text-align:center;}"
        "input[type=text],input[type=number],select{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #2a2a50;background:#0a0a1e;color:#e0e0f0;font-size:15px;margin-bottom:10px;outline:none;}"
        "input:focus,select:focus{border-color:#7c6af7;}"
        ".btn-primary{width:100%;padding:12px;border:none;border-radius:10px;background:linear-gradient(135deg,#7c6af7,#5b4de8);color:#fff;font-size:15px;font-weight:700;cursor:pointer;transition:all .2s;}"
        ".btn-primary:active{transform:scale(0.98);opacity:0.8;}"
        ".btn-led-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;}"
        ".btn-led{padding:12px 5px;border-radius:10px;border:1px solid #2a2a50;background:#1a1a35;color:#e0e0f0;font-weight:700;cursor:pointer;transition:all .15s;}"
        ".btn-led:active{background:#7c6af7;color:#fff;}"
        ".action-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;width:100%;}"
        ".act-btn{padding:10px 4px;font-size:11px;font-weight:600;background:#0a0a1e;border:1px solid #1e1e3a;border-radius:8px;color:#444;cursor:default;}"
        ".status-badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:11px;font-weight:700;background:#1e1e3a;color:#7c6af7;margin-bottom:6px;}"
        ".err-msg{color:#f7736a;font-size:12px;margin-top:6px;text-align:center;}"
        ".slider{-webkit-appearance:none;width:100%;height:6px;background:#1e1e3a;border-radius:5px;outline:none;margin:10px 0;}"
        ".slider::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:18px;height:18px;background:#7c6af7;cursor:pointer;border-radius:50%;transition:all .15s;}"
        ".slider::-webkit-slider-thumb:hover{transform:scale(1.2);background:#5b4de8;}"
        "</style>"
        "</head><body>"
        "<div class='content'>"
        "<div class='card' style='text-align:center;'>"
        "<span class='status-badge' id='conn-badge'>●&nbsp;Online</span>"
        "<div class='time-big' id='clock'>--:--:--</div>"
        "<div class='epoch'>Epoch: <span id='epoch'>-</span>&nbsp;&nbsp;Synced: <span id='synced'>no</span></div>"
        "</div>"

        "<div class='card'>"
        "<h2>GPIO LED Control</h2>"
        "<div class='btn-led-grid'>"
        "<button class='btn-led' onclick='led(\"l1on\")'>ON</button>"
        "<button class='btn-led' onclick='led(\"l1off\")'>OFF</button>"
        "<button class='btn-led' onclick='led(\"toggle\")'>TOGGLE</button>"
        "</div>"
        "</div>"

        "<div class='card'>"
        "<h2>Servo Control</h2>"
        "<div style='margin-bottom:15px;'>"
        "  <div style='display:flex;justify-content:space-between;margin-bottom:5px;'>"
        "    <label>Servo 1</label>"
        "    <span id='s1-val'>121\xc2\xb0</span>"
        "  </div>"
        "  <input type='range' min='0' max='180' value='121' class='slider' id='s1-slide' oninput='sv(1,this.value)'>"
        "  <div class='btn-led-grid' style='margin-top:5px;'>"
        "    <button class='btn-led' onclick='dog(\"s1on\")'>ON</button>"
        "    <button class='btn-led' onclick='dog(\"s1off\")'>OFF</button>"
        "  </div>"
        "</div>"
        "<div>"
        "  <div style='display:flex;justify-content:space-between;margin-bottom:5px;'>"
        "    <label>Servo 2</label>"
        "    <span id='s2-val'>121\xc2\xb0</span>"
        "  </div>"
        "  <input type='range' min='0' max='180' value='121' class='slider' id='s2-slide' oninput='sv(2,this.value)'>"
        "  <div class='btn-led-grid' style='margin-top:5px;'>"
        "    <button class='btn-led' onclick='dog(\"s2on\")'>ON</button>"
        "    <button class='btn-led' onclick='dog(\"s2off\")'>OFF</button>"
        "  </div>"
        "</div>"
        "</div>"

        "<div class='card'>"
        "<h2>Schedule Action</h2>"
        "<div style='display:flex;gap:10px;'>"
        "<input type='number' id='sched-delay' placeholder='Delay (sec)' value='5' style='flex:1; margin-bottom:0;'>"
        "<select id='sched-action' style='flex:2; padding:10px 12px; border-radius:8px; border:1px solid #2a2a50; background:#0a0a1e; color:#e0e0f0; outline:none;'>"
        "<option value='toggle'>Toggle LED</option><option value='l1on'>LED ON</option><option value='l1off'>LED OFF</option><option value='hi'>Say Hi</option><option value='bark'>Bark</option>"
        "</select>"
        "</div>"
        "<button class='btn-primary' onclick='scheduleAction()' style='margin-top:10px;'>Schedule</button>"
        "<div class='err-msg' id='sched-err'></div>"
        "</div>"

        "<div class='card' id='tts-card'>"
        "<h2>Text to Speech</h2>"
        "<input type='text' id='say' placeholder='Broadcast text to browsers...'>"
        "<button class='btn-primary' onclick='sendTTS()'>Speak</button>"
        "<div class='err-msg' id='tts-err'></div>"
        "</div>"

        "<div class='card'>"
        "<h2>Dog Actions</h2>"
        "<div class='action-grid' id='action-grid'></div>"
        "</div>"

#ifndef RF_FREQ_MHZ
#define RF_FREQ_MHZ 433
#endif
#define _RF_STR(x) #x
#define RF_FREQ_STR(x) _RF_STR(x)

        /* ── RF Radio Card ──────────────────────────────────────────────── */
        "<div class='card' id='rf-card'>"
        "<h2>\xf0\x9f\x93\xa1 " RF_FREQ_STR(RF_FREQ_MHZ) " MHz RF Radio</h2>"
        "<div style='display:flex;gap:10px;margin-bottom:12px;align-items:center;'>"
        "  <button class='btn-primary' id='rf-listen-btn' onclick='rfToggleListen()' style='flex:1;background:linear-gradient(135deg,#1b8f5e,#0d6644);'>\xe2\x97\x8f\xc2\xa0 Start Listening</button>"
        "  <span id='rf-listen-badge' style='font-size:11px;font-weight:700;padding:4px 10px;border-radius:20px;background:#1e1e3a;color:#555;white-space:nowrap;'>IDLE</span>"
        "</div>"
        "<div style='margin-bottom:10px;'>"
        "  <div style='display:flex;gap:8px;'>"
        "    <input type='text' id='rf-code' placeholder='Code: decimal (5584140) or 0x hex' style='flex:2;margin-bottom:0;'>"
        "    <input type='number' id='rf-bits' placeholder='Bits' value='24' style='flex:1;margin-bottom:0;'>"
        "    <input type='number' id='rf-proto' placeholder='Proto' value='1' style='flex:1;margin-bottom:0;'>"
        "    <input type='number' id='rf-pulse' placeholder='\xc2\xb5s' value='185' style='flex:1;margin-bottom:0;'>"
        "  </div>"
        "  <button class='btn-primary' onclick='rfSend()' style='margin-top:8px;background:linear-gradient(135deg,#7c3af7,#5b2de8);'>\xe2\x9a\xa1\xef\xb8\x8f Transmit</button>"
        "  <div class='err-msg' id='rf-send-err'></div>"
        "</div>"
        "<div style='font-size:11px;font-weight:700;color:#7c6af7;text-transform:uppercase;letter-spacing:1px;margin-bottom:6px;'>Signal Log</div>"
        "<div id='rf-log' style='background:#060610;border:1px solid #1e1e3a;border-radius:8px;padding:10px 12px;height:180px;overflow-y:auto;font-family:monospace;font-size:12px;color:#00e5a0;line-height:1.7;'>"
        "  <span style='color:#444;'>Waiting for signals...</span>"
        "</div>"
        "<div style='display:flex;gap:8px;margin-top:8px;'>"
        "  <button onclick='rfClearLog()' style='flex:1;padding:7px;border-radius:8px;border:1px solid #2a2a50;background:#0a0a1e;color:#888;font-size:11px;cursor:pointer;'>Clear Log</button>"
        "  <div id='rf-poll-status' style='flex:2;font-size:11px;color:#444;display:flex;align-items:center;padding:0 8px;'></div>"
        "</div>"
        "</div>"

        /* ── Photodetector RF Relay Card ───────────────────────────────── */
        "<div class='card' id='rf-relay-card'>"
        "<h2>\xf0\x9f\x93\xa1 Photodetector RF Relay</h2>"
        "<div style='margin-bottom:10px;font-size:12px;color:#888;'>Relay RF code to SpeakerBot API (e.g. " RF_RELAY_TARGET "). Disabled by default.</div>"
        "<div style='display:flex;align-items:center;gap:10px;margin-bottom:10px;'>"
        "  <input type='checkbox' id='rf-relay-enable' style='width:18px;height:18px;accent-color:#00e5a0;cursor:pointer;'>"
        "  <label for='rf-relay-enable' style='font-size:13px;font-weight:600;color:#e0e0f0;cursor:pointer;'>Enable RF Relay Mode</label>"
        "</div>"
        "<div style='margin-bottom:10px;'>"
        "  <label style='display:block;font-size:11px;color:#a0a0d0;margin-bottom:4px;'>Target SpeakerBot Host / Endpoint</label>"
        "  <input type='text' id='rf-relay-host' placeholder='" RF_RELAY_TARGET "' value='" RF_RELAY_TARGET "' style='margin-bottom:0;'>"
        "</div>"
        "<div style='display:flex;gap:8px;'>"
        "  <button class='btn-primary' onclick='saveRfRelay()' style='flex:2;background:linear-gradient(135deg,#1b8f5e,#0d6644);'>Save Relay Settings</button>"
        "  <button class='btn-primary' onclick='testRfRelay()' style='flex:1;background:linear-gradient(135deg,#7c6af7,#5b2de8);'>\xe2\x9a\xa1\xc2\xa0Test Bark</button>"
        "</div>"
        "<div id='rf-relay-status' style='font-size:11px;font-weight:600;color:#00e5a0;margin-top:6px;min-height:16px;'></div>"
        "<div class='err-msg' id='rf-relay-err' style='margin-top:4px;'></div>"
        "</div>";

        /* ── RF Outlets card injected here dynamically ─────────────────── */

    /* ── Part 2: OTA card + WiFi setup card + all JavaScript ───────────── */
    static const char html_post[] =
        "<div class='card' id='wifi-card'>"
        "<h2>\xf0\x9f\x93\xa1 WiFi Setup</h2>"
        "<div id='wifi-status' style='margin-bottom:12px;'></div>"
        "<div id='saved-nets' style='margin-bottom:12px;'></div>"
        "<button class='btn-primary' onclick='scanWifi()' style='background:linear-gradient(135deg,#1a1a35,#20203d);border:1px solid #2a2a50;margin-bottom:10px;'>Scan for Networks</button>"
        "<div id='scan-results' style='margin-bottom:10px;'></div>"
        "<div style='border-top:1px solid #1e1e3a;padding-top:12px;margin-top:8px;'>"
        "<div style='font-size:11px;color:#666;font-weight:700;margin-bottom:8px;'>ADD NETWORK MANUALLY</div>"
        "<input type='text' id='wifi-ssid' placeholder='WiFi Name (SSID)' style='font-size:16px;padding:14px 12px;'>"
        "<div style='position:relative;margin-bottom:10px;'>"
        "<input type='password' id='wifi-pass' placeholder='Password' autocomplete='current-password' style='width:100%;font-size:16px;padding:14px 48px 14px 12px;border-radius:8px;border:1px solid #2a2a50;background:#0a0a1e;color:#e0e0f0;outline:none;box-sizing:border-box;'>"
        "<button type=\"button\" onclick=\"var i=document.getElementById('wifi-pass');i.type=i.type==='password'?'text':'password';\" style='position:absolute;right:0;top:0;bottom:0;width:44px;background:none;border:none;color:#666;font-size:20px;cursor:pointer;'>\xf0\x9f\x91\x81</button>"
        "</div>"
        "<button class='btn-primary' onclick='saveWifi()' style='font-size:16px;padding:14px;'>Connect & Reboot</button>"
        "<div class='err-msg' id='wifi-err'></div>"
        "</div>"
        "</div>"

        "<div class='card'>"
        "<h2>OTA Firmware Update</h2>"
        "<input type='file' id='ota-file' accept='.bin' style='color:#a0a0d0;margin-bottom:10px;width:100%;'>"
        "<div style='background:#0a0a1e;border-radius:6px;height:8px;margin-bottom:8px;overflow:hidden;'>"
        "<div id='ota-bar' style='width:0%;height:100%;background:linear-gradient(90deg,#00e5a0,#7c6af7);transition:width .3s;'></div>"
        "</div>"
        "<button class='btn-primary' id='ota-btn' onclick='doOTA()'>Flash Firmware</button>"
        "<div class='err-msg' id='ota-msg'></div>"
        "</div>"

        "</div>" /* end .content */
        "<script>"
        "function scanWifi(){"
        "  var sr=document.getElementById('scan-results');"
        "  sr.innerHTML='<div style=\"color:#666;font-size:12px;\">Scanning...</div>';"
        "  fetch('/wifi_scan').then(function(r){return r.json();}).then(function(d){"
        "    var h='';"
        "    if(d.networks&&d.networks.length){"
        "      d.networks.forEach(function(n){"
        "        h+='<div style=\"display:flex;justify-content:space-between;align-items:center;padding:8px 10px;margin:4px 0;background:#0a0a1e;border-radius:8px;border:1px solid #1e1e3a;cursor:pointer;\" onclick=\"document.getElementById(\\'wifi-ssid\\').value=\\''+n.ssid.replace(/'/g,'')+'\\';\">';"
        "        h+='<span style=\"color:#e0e0f0;font-size:13px;\">'+n.ssid+'</span>';"
        "        h+='<span style=\"color:#666;font-size:11px;\">'+n.rssi+'dBm</span></div>';"
        "      });"
        "    }else{ h='<div style=\"color:#666;font-size:12px;\">No networks found</div>'; }"
        "    sr.innerHTML=h;"
        "  }).catch(function(e){sr.innerHTML='<div class=\"err-msg\">Scan error: '+e+'</div>';});"
        "}"
        "function saveWifi(){"
        "  var s=document.getElementById('wifi-ssid').value.trim();"
        "  var p=document.getElementById('wifi-pass').value;"
        "  var e=document.getElementById('wifi-err');"
        "  if(!s){e.textContent='Enter an SSID';return;}"
        "  e.style.color='#a0a0d0';e.textContent='Saving & rebooting...';"
        "  fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,pass:p})})"
        "    .then(function(r){return r.json();}).then(function(d){"
        "      if(d.ok){e.style.color='#00e5a0';e.textContent='Saved! Rebooting in 3s...';}"
        "      else{e.style.color='#f7736a';e.textContent='Error: '+(d.error||'unknown');}"
        "    }).catch(function(x){e.style.color='#f7736a';e.textContent='Network error: '+x;});"
        "}"
        "function loadSavedNets(){"
        "  fetch('/wifi').then(function(r){return r.json();}).then(function(d){"
        "    var h='',ws=document.getElementById('wifi-status');"
        "    if(d.ap_mode){"
        "      ws.innerHTML='<div style=\"padding:8px 12px;background:#2a1a1a;border:1px solid #5a2a2a;border-radius:8px;color:#f7a56a;font-size:13px;\">\xe2\x9a\xa0\xef\xb8\x8f Running in AP mode \xe2\x80\x94 add a WiFi network below to connect.</div>';"
        "    }else{"
        "      ws.innerHTML='<div style=\"padding:8px 12px;background:#1a2a1a;border:1px solid #2a5a2a;border-radius:8px;color:#00e5a0;font-size:13px;\">\xe2\x9c\x93 Connected to WiFi</div>';"
        "    }"
        "    if(d.saved&&d.saved.length){"
        "      h='<div style=\"font-size:11px;color:#666;margin-bottom:6px;font-weight:700;\">SAVED NETWORKS</div>';"
        "      d.saved.forEach(function(n,i){"
        "        h+='<div style=\"display:flex;justify-content:space-between;align-items:center;padding:6px 10px;margin:3px 0;background:#0a0a1e;border-radius:6px;border:1px solid #1e1e3a;\">';"
        "        h+='<span style=\"color:#a0a0d0;font-size:12px;\">'+n+'</span>';"
        "        h+='<button onclick=\"deleteNet('+i+')\" style=\"background:none;border:none;color:#666;cursor:pointer;font-size:16px;padding:0 4px;\">&times;</button></div>';"
        "      });"
        "    }"
        "    document.getElementById('saved-nets').innerHTML=h;"
        "  }).catch(function(){});"
        "}"
        "function deleteNet(idx){"
        "  fetch('/wifi?delete='+idx,{method:'DELETE'}).then(function(){loadSavedNets();}).catch(function(){});"
        "}"
        "loadSavedNets();"
        "function led(a){fetch('/'+a);}"
        "function dog(v){fetch('/'+v);}"
        "function sv(n,a){"
        "document.getElementById('s'+n+'-val').textContent=a+'\xc2\xb0';"
        "fetch('/s'+n+'_'+a);"
        "}"
        "function scheduleAction(){"
        "var d=document.getElementById('sched-delay').value;"
        "var a=document.getElementById('sched-action').value;"
        "fetch('/schedule?action='+a+'&delay='+d).then(function(r){return r.json();})"
        ".then(function(j){ document.getElementById('sched-err').textContent='Scheduled '+a+' in '+d+'s'; })"
        ".catch(function(e){ document.getElementById('sched-err').textContent=e; });"
        "}"
        "var ACTIONS={'hi':'Say Hi','s1on':'S1 ON','s1off':'S1 OFF','s2on':'S2 ON','s2off':'S2 OFF'};"
        "(function(){"
        "var g=document.getElementById('action-grid');"
        "for(var k in ACTIONS){"
        "var b=document.createElement('button');"
        "b.className='act-btn';b.textContent=ACTIONS[k];"
        "b.style.cursor='pointer';b.style.color='#a0a0d0';"
        "(function(val){b.onclick=function(){dog(val);};})(k);"
        "g.appendChild(b);"
        "}"
        "})();"
        "var timeOffset=0,isSynced=false;"
        "function syncT(){"
        "fetch('/time').then(function(r){return r.json();}).then(function(d){"
        "if(d.synced){"
        "  timeOffset = (d.epoch * 1000) - Date.now();"
        "  isSynced = true;"
        "  document.getElementById('synced').textContent='yes';"
        "}else{"
        "  fetch('/sync_time?epoch='+Math.floor(Date.now()/1000));"
        "  timeOffset = 0; isSynced = true;"
        "  document.getElementById('synced').textContent='local';"
        "}"
        "}).catch(function(){});"
        "}"
        "syncT(); setInterval(syncT, 10000);"
        "setInterval(function(){"
        "var d=new Date(Date.now()+timeOffset);"
        "var f=d.getFullYear()+'-'+('0'+(d.getMonth()+1)).slice(-2)+'-'+('0'+d.getDate()).slice(-2)+' '+"
        "('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)+':'+('0'+d.getSeconds()).slice(-2);"
        "document.getElementById('clock').textContent=f;"
        "document.getElementById('epoch').textContent=Math.floor(d.getTime()/1000);"
        "},100);"
        "function sendTTS(){"
        "var t=document.getElementById('say').value.trim();"
        "if(!t)return;"
        "var err=document.getElementById('tts-err');"
        "err.textContent='Sending...';"
        "fetch('/tts?say='+encodeURIComponent(t)).then(function(){err.textContent='';}).catch(function(e){err.textContent=e;});"
        "}"
        "function initSSE(){"
        "var es=new EventSource('/events');"
        "es.onerror=function(){es.close();setTimeout(initSSE,8000);};"
        "}"
        "initSSE();"
        "function doOTA(){"
        "var f=document.getElementById('ota-file').files[0];"
        "var msg=document.getElementById('ota-msg');"
        "var bar=document.getElementById('ota-bar');"
        "var btn=document.getElementById('ota-btn');"
        "if(!f){msg.textContent='Pick a .bin file first';return;}"
        "btn.disabled=true;btn.style.opacity='.5';"
        "msg.style.color='#a0a0d0';msg.textContent='Uploading...';"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/ota',true);"
        "xhr.upload.onprogress=function(e){"
        "  if(e.lengthComputable){"
        "    var pct=Math.round(e.loaded/e.total*100);"
        "    bar.style.width=pct+'%';"
        "    msg.textContent='Uploading... '+pct+'%';"
        "  }"
        "};"
        "xhr.onload=function(){"
        "  bar.style.width='100%';"
        "  if(xhr.status===200){"
        "    msg.style.color='#00e5a0';"
        "    msg.textContent='OTA OK! Rebooting...';"
        "    setTimeout(function(){location.reload();},10000);"
        "  } else {"
        "    msg.style.color='#f7736a';"
        "    msg.textContent='OTA failed: HTTP '+xhr.status;"
        "    btn.disabled=false;btn.style.opacity='1';"
        "  }"
        "};"
        "xhr.send(f);"
        "}"
        /* ── RF Radio + Outlets JavaScript ──────────────────────────────── */
        "var rfListening=false,rfPollTimer=null,rfLogCount=0;"
        "function rfToggleListen(){"
        "  if(!rfListening){"
        "    fetch('/rf/listen/start').then(function(){"
        "      rfListening=true;"
        "      document.getElementById('rf-listen-btn').textContent='\xe2\x96\xa0\xc2\xa0 Stop Listening';"
        "      document.getElementById('rf-listen-btn').style.background='linear-gradient(135deg,#8f1b1b,#6a0d0d)';"
        "      var badge=document.getElementById('rf-listen-badge');"
        "      badge.textContent='\xf0\x9f\x94\xb4 LIVE';"
        "      badge.style.color='#f7736a';badge.style.background='rgba(247,115,106,0.15)';"
        "      var log=document.getElementById('rf-log');"
        "      log.innerHTML='<div style=\"color:#888;font-style:italic;\">Listening started...</div>';"
        "      rfPollTimer=setInterval(rfPoll,800);"
        "    }).catch(function(e){document.getElementById('rf-poll-status').textContent='Error: '+e;});"
        "  } else {"
        "    fetch('/rf/listen/stop').then(function(){"
        "      rfListening=false;"
        "      clearInterval(rfPollTimer);rfPollTimer=null;"
        "      document.getElementById('rf-listen-btn').textContent='\xe2\x97\x8f\xc2\xa0 Start Listening';"
        "      document.getElementById('rf-listen-btn').style.background='linear-gradient(135deg,#1b8f5e,#0d6644)';"
        "      var badge=document.getElementById('rf-listen-badge');"
        "      badge.textContent='STOPPED';badge.style.color='#555';badge.style.background='#1e1e3a';"
        "      document.getElementById('rf-poll-status').textContent='';"
        "    });"
        "  }"
        "}"
        "function rfPoll(){"
        "  fetch('/rf/poll').then(function(r){return r.json();}).then(function(d){"
        "    var log=document.getElementById('rf-log');"
        "    var status=document.getElementById('rf-poll-status');"
        "    if(d.packets&&d.packets.length>0){"
        "      d.packets.forEach(function(p){"
        "        rfLogCount++;"
        "        var ts=new Date().toLocaleTimeString();"
        "        var line=document.createElement('div');"
        "        line.style.borderBottom='1px solid #111128';"
        "        line.style.paddingBottom='2px';"
        "        var tag=p.relayed?' <span style=\"color:#00e5a0;font-weight:700;background:rgba(0,229,160,0.15);padding:1px 5px;border-radius:4px;\">\xe2\x9a\xa1\xc2\xa0RELAYED</span>':'';"
        "        line.innerHTML='<span style=color:#444>['+ts+']</span> '"
        "          +'<span style=color:#00e5a0;font-weight:700>0x'+p.code+'</span>'"
        "          +' <span style=color:#7c6af7>'+p.bits+'b</span>'"
        "          +' proto:<span style=color:#a0a0d0>'+p.proto+'</span>'"
        "          +' pulse:<span style=color:#888>'+p.pulse+'\xc2\xb5s</span>'+tag;"
        "        log.appendChild(line);"
        "        log.scrollTop=log.scrollHeight;"
        "      });"
        "      status.textContent='Edges: '+d.edges+' | '+rfLogCount+' signals captured';"
        "    } else {"
        "      status.textContent='Edges: '+d.edges+' | '+rfLogCount+' signals captured';"
        "    }"
        "  }).catch(function(){});"
        "}"
        "function rfSend(){"
        "  var code=document.getElementById('rf-code').value.trim();"
        "  var bits=document.getElementById('rf-bits').value||'24';"
        "  var proto=document.getElementById('rf-proto').value||'1';"
        "  var pulse=document.getElementById('rf-pulse').value||'185';"
        "  var err=document.getElementById('rf-send-err');"
        "  if(!code){err.textContent='Enter a code (decimal or 0x hex)';return;}"
        "  err.textContent='Sending...';"
        "  err.style.color='#a0a0d0';"
        "  fetch('/rf/send?code='+encodeURIComponent(code)+'&bits='+bits+'&proto='+proto+'&pulse='+pulse)"
        "    .then(function(r){return r.json();})"
        "    .then(function(d){err.style.color='#00e5a0';err.textContent=d.ok?'Sent 0x'+d.sent_hex+'!':'Error: '+JSON.stringify(d);})"
        "    .catch(function(e){err.style.color='#f7736a';err.textContent=e;});"
        "}"
        "function rfClearLog(){"
        "  document.getElementById('rf-log').innerHTML='<span style=color:#444>Log cleared.</span>';"
        "  rfLogCount=0;"
        "  document.getElementById('rf-poll-status').textContent='';"
        "}"
        /* rfOutlet: send ON (on=1) or OFF (on=0) for an outlet code.
         * Always uses proto=1, 24 bits, 185µs — matching the remotes. */
        "function rfOutlet(code,on){"
        "  var st=document.getElementById('outlet-status');"
        "  st.textContent='Sending...';"
        "  fetch('/rf/send?code='+code+'&bits=24&proto=1&pulse=185')"
        "    .then(function(r){return r.json();})"
        "    .then(function(d){"
        "      st.style.color=d.ok?'#00e5a0':'#f7736a';"
        "      st.textContent=d.ok?(on?'\xe2\x9c\x93 ON sent (0x'+d.sent_hex+')':'\xe2\x9c\x93 OFF sent (0x'+d.sent_hex+')'):'Error';"
        "      setTimeout(function(){st.textContent='';},3000);"
        "    }).catch(function(e){st.style.color='#f7736a';st.textContent=e;});"
        "}"
        "function loadRfRelay(){"
        "  fetch('/rf/relay').then(function(r){return r.json();}).then(function(d){"
        "    if(document.getElementById('rf-relay-enable')) document.getElementById('rf-relay-enable').checked=!!d.enabled;"
        "    if(d.host && document.getElementById('rf-relay-host') && document.activeElement !== document.getElementById('rf-relay-host')) document.getElementById('rf-relay-host').value=d.host;"
        "    var statusDiv=document.getElementById('rf-relay-status');"
        "    if(statusDiv && d.last_event) statusDiv.textContent=d.last_event;"
        "  }).catch(function(){});"
        "}"
        "function saveRfRelay(){"
        "  var en=document.getElementById('rf-relay-enable').checked?1:0;"
        "  var h=document.getElementById('rf-relay-host').value.trim();"
        "  var err=document.getElementById('rf-relay-err');"
        "  if(!h){err.textContent='Enter target host';return;}"
        "  err.style.color='#a0a0d0';err.textContent='Saving...';"
        "  fetch('/rf/relay?enabled='+en+'&host='+encodeURIComponent(h))"
        "    .then(function(r){return r.json();})"
        "    .then(function(d){"
        "      err.style.color='#00e5a0';"
        "      err.textContent=d.enabled?'Relay ENABLED -> '+d.host:'Relay DISABLED';"
        "      loadRfRelay();"
        "    }).catch(function(e){err.style.color='#f7736a';err.textContent=e;});"
        "}"
        "function testRfRelay(){"
        "  var err=document.getElementById('rf-relay-err');"
        "  err.style.color='#a0a0d0';err.textContent='Triggering test...';"
        "  fetch('/rf/relay/test')"
        "    .then(function(r){return r.json();})"
        "    .then(function(d){"
        "      err.style.color='#00e5a0';"
        "      err.textContent='\xe2\x9a\xa1\xc2\xa0Test bark triggered!';"
        "      setTimeout(function(){err.textContent='';loadRfRelay();},1500);"
        "    }).catch(function(e){err.style.color='#f7736a';err.textContent=e;});"
        "}"
        "loadRfRelay();"
        "setInterval(loadRfRelay,3000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");

    /* Send Part 1 — static HTML up to and including the RF Radio card */
    httpd_resp_send_chunk(req, html_pre, HTTPD_RESP_USE_STRLEN);

    /* Inject the RF Outlets card (rows generated from rf_outlets_config.h) */
#ifdef RF_RX_GPIO
    rf_outlets_card_send(req);
#endif

    /* Send Part 2 — OTA card + all JavaScript */
    httpd_resp_send_chunk(req, html_post, HTTPD_RESP_USE_STRLEN);

    /* Terminate chunked transfer */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── RF Outlets card (generated from rf_outlets_config.h) ────────────────
 * Each outlet row is sent as a series of small literal chunks plus one tiny
 * snprintf for the onclick code number.  No large stack buffer needed.      */
#ifdef RF_RX_GPIO
static void rf_outlets_card_send(httpd_req_t *req)
{
    httpd_resp_send_chunk(req,
        "<div class='card' id='outlets-card'>"
        "<h2>&#x1F50C; RF Outlets</h2>"
        "<div id='outlet-status' style='font-size:11px;color:#00e5a0;"
            "min-height:16px;margin-bottom:8px;'></div>",
        HTTPD_RESP_USE_STRLEN);

    /* Styles shared by both ON and OFF buttons (only the gradient differs) */
    for (int i = 0; i < RF_OUTLET_COUNT; i++) {
        char tmp[64];   /* large enough for onclick='rfOutlet(NNNNNNNN,X)' */

        /* Row open + label */
        httpd_resp_send_chunk(req,
            "<div style='display:flex;align-items:center;gap:8px;margin-bottom:8px;'>"
            "<span style='flex:1;font-size:13px;font-weight:600;color:#a0a0d0;'>",
            HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, s_outlets[i].label, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, "</span>", HTTPD_RESP_USE_STRLEN);

        /* ON button */
        snprintf(tmp, sizeof(tmp), "<button onclick='rfOutlet(%lu,1)' ",
                 (unsigned long)s_outlets[i].on_code);
        httpd_resp_send_chunk(req, tmp, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req,
            "style='flex:1;padding:9px;border:none;border-radius:8px;"
            "background:linear-gradient(135deg,#1b8f5e,#0d6644);"
            "color:#fff;font-weight:700;cursor:pointer;'>ON</button>",
            HTTPD_RESP_USE_STRLEN);

        /* OFF button */
        snprintf(tmp, sizeof(tmp), "<button onclick='rfOutlet(%lu,0)' ",
                 (unsigned long)s_outlets[i].off_code);
        httpd_resp_send_chunk(req, tmp, HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req,
            "style='flex:1;padding:9px;border:none;border-radius:8px;"
            "background:linear-gradient(135deg,#8f1b1b,#6a0d0d);"
            "color:#fff;font-weight:700;cursor:pointer;'>OFF</button>",
            HTTPD_RESP_USE_STRLEN);

        httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "</div>", HTTPD_RESP_USE_STRLEN);
}
#endif


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

// Quick-action: now async — returns immediately
static esp_err_t quick_action_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    ESP_LOGI("WEB", "Quick action URI: %s", uri);
    // Strip leading '/' and dispatch
    if (uri[0] == '/') uri++;
    execute_named_action(uri);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Servo endpoints removed for RFbot.

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
// Removed eye_mood_handler and oled_text_handler

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
//  433 MHz RF API handlers
// ══════════════════════════════════════════════════════════════
#ifdef RF_RX_GPIO

// GET /rf/listen/start  — enable listen mode (buffers all received packets)
static esp_err_t rf_listen_start_handler(httpd_req_t *req) {
    rf_listen_start();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"listen\":\"started\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /rf/listen/stop  — disable listen mode
static esp_err_t rf_listen_stop_handler(httpd_req_t *req) {
    rf_listen_stop();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"listen\":\"stopped\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /rf/poll  — drain the listen ring buffer, return JSON array of packets
// Returns: {"listening":true,"packets":[{"code":"1A2B","bits":24,"proto":1,"pulse":350}]}
static esp_err_t rf_poll_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "listening", rf_listen_active());

    extern volatile uint32_t rf_isr_edge_count;
    cJSON_AddNumberToObject(root, "edges", rf_isr_edge_count);

    cJSON *pkts = rf_listen_get_packets();
    cJSON_AddItemToObject(root, "packets", pkts);

    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (js) {
        httpd_resp_send(req, js, HTTPD_RESP_USE_STRLEN);
        free(js);
    } else {
        httpd_resp_send(req, "{\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// GET /rf/send?code=<hex>&bits=<n>&proto=<p>&pulse=<us>
// Send a 433 MHz code.  All params except 'code' are optional.
// Returns: {"ok":true,"code":"1A2B","bits":24,"proto":1,"pulse":350}
static esp_err_t rf_send_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    char qs[128];
    char code_str[16] = {0};
    int  bits  = 24;
    int  proto = 1;
#ifndef RF_PULSE_WIDTH
#define RF_PULSE_WIDTH 185
#endif
    int  pulse = RF_PULSE_WIDTH;

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char p[32];
        if (httpd_query_key_value(qs, "code",  p, sizeof(p)) == ESP_OK) strncpy(code_str, p, sizeof(code_str) - 1);
        if (httpd_query_key_value(qs, "bits",  p, sizeof(p)) == ESP_OK) bits  = atoi(p);
        if (httpd_query_key_value(qs, "proto", p, sizeof(p)) == ESP_OK) proto = atoi(p);
        if (httpd_query_key_value(qs, "pulse", p, sizeof(p)) == ESP_OK) pulse = atoi(p);
    }

    if (!code_str[0]) {
        httpd_resp_send(req, "{\"error\":\"Missing ?code=\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Auto-detect base: 0x/0X prefix → hex, otherwise → decimal.
     * This fixes the bug where entering a decimal code (e.g. 5584140) was
     * silently parsed as hex (0x5584140 = 89587008) and the wrong code was sent. */
    uint32_t code_val;
    if ((code_str[0] == '0') && (code_str[1] == 'x' || code_str[1] == 'X')) {
        code_val = (uint32_t)strtoul(code_str + 2, NULL, 16);
    } else {
        /* Check if it looks like a pure hex string (contains a-f/A-F) */
        int has_alpha = 0;
        for (int i = 0; code_str[i]; i++) {
            char c = code_str[i];
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) { has_alpha = 1; break; }
        }
        code_val = has_alpha ? (uint32_t)strtoul(code_str, NULL, 16)
                             : (uint32_t)strtoul(code_str, NULL, 10);
    }

    rf_send_full(code_val, (unsigned)bits, proto, pulse);
    ESP_LOGI("RF_API", "TX 0x%lX (input='%s') bits=%d proto=%d pulse=%d",
             (unsigned long)code_val, code_str, bits, proto, pulse);

    char resp[160];
    int len = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"code\":\"%s\",\"sent_hex\":\"%lX\",\"bits\":%d,\"proto\":%d,\"pulse\":%d}",
        code_str, (unsigned long)code_val, bits, proto, pulse);
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

// GET /rf/status  — returns RF module state (listen mode, learn mode)
static esp_err_t rf_status_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"listening\":%s,\"learning\":%s,\"tx_gpio\":%d,\"rx_gpio\":%d}",
        rf_listen_active() ? "true" : "false",
        rf_learn_active()  ? "true" : "false",
        RF_TX_GPIO, RF_RX_GPIO);
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static void url_decode_in_place(char *str) {
    if (!str) return;
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int h1 = src[1];
            int h2 = src[2];
            int v1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') :
                     (h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) :
                     (h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) : -1;
            int v2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') :
                     (h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) :
                     (h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) : -1;
            if (v1 >= 0 && v2 >= 0) {
                *dst++ = (char)((v1 << 4) | v2);
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

// GET /rf/relay?enabled=<0|1>&host=<target> — get or set RF relay config
static esp_err_t rf_relay_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    char qs[128] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char enabled_str[16] = {0};
        char host_str[64]   = {0};
        bool update = false;
        bool enabled = rf_relay_is_enabled();
        char cur_host[64] = {0};
        rf_relay_get_config(NULL, cur_host, sizeof(cur_host), NULL, 0);

        if (httpd_query_key_value(qs, "enabled", enabled_str, sizeof(enabled_str)) == ESP_OK) {
            enabled = (atoi(enabled_str) != 0 || strcasecmp(enabled_str, "true") == 0);
            update = true;
        }
        if (httpd_query_key_value(qs, "host", host_str, sizeof(host_str)) == ESP_OK) {
            url_decode_in_place(host_str);
            strncpy(cur_host, host_str, sizeof(cur_host) - 1);
            update = true;
        }

        if (update) {
            rf_relay_set_config(enabled, cur_host);
        }
    }

    bool cur_enabled = false;
    char cur_host[64] = {0};
    char last_event[128] = {0};
    rf_relay_get_config(&cur_enabled, cur_host, sizeof(cur_host), last_event, sizeof(last_event));

    char resp[256];
    int len = snprintf(resp, sizeof(resp),
        "{\"enabled\":%s,\"host\":\"%s\",\"last_event\":\"%s\"}",
        cur_enabled ? "true" : "false", cur_host, last_event);
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

// GET /rf/relay/test — manually trigger the relay bark request for testing
static esp_err_t rf_relay_test_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    rf_relay_trigger();
    httpd_resp_send(req, "{\"ok\":true,\"testing\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

#endif /* RF_RX_GPIO */


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
    config.max_uri_handlers = 100;     // increased for new endpoints
    config.recv_wait_timeout  = 300;  // 300 s — allows large OTA binary uploads
    config.send_wait_timeout  = 10;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard; // enables /sendtts:* pattern

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
        { "/events",    HTTP_GET,  sse_handler,            NULL },
        { "/sync_time", HTTP_GET,  sync_time_handler,      NULL },
        // ... (rest of quick actions)
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

#ifdef DISP_MOSI_GPIO
        { "/audio",          HTTP_POST, audio_post_handler,     NULL },
        { "/audio",          HTTP_OPTIONS, cors_options_handler,NULL },
#endif
        { "/ota",            HTTP_POST, ota_post_handler,       NULL },
        { "/ota",            HTTP_OPTIONS, cors_options_handler,NULL },
        { "/servo",          HTTP_GET,  servo_handler,          NULL },
        { "/s1_*",           HTTP_GET,  servo_angle_uri_handler,NULL },
        { "/s2_*",           HTTP_GET,  servo_angle_uri_handler,NULL },
#ifdef RF_RX_GPIO
        // ── 433 MHz RF API ───────────────────────────────────────────────────
        { "/rf/listen/start",HTTP_GET,  rf_listen_start_handler,NULL },
        { "/rf/listen/stop", HTTP_GET,  rf_listen_stop_handler, NULL },
        { "/rf/poll",        HTTP_GET,  rf_poll_handler,        NULL },
        { "/rf/send",        HTTP_GET,  rf_send_handler,        NULL },
        { "/rf/status",      HTTP_GET,  rf_status_handler,      NULL },
        { "/rf/relay",       HTTP_GET,  rf_relay_handler,       NULL },
        { "/rf/relay/test",  HTTP_GET,  rf_relay_test_handler,  NULL },
#endif
        // WiFi provisioning endpoints
        { "/wifi",      HTTP_GET,    wifi_get_handler,     NULL },
        { "/wifi",      HTTP_POST,   wifi_post_handler,    NULL },
        { "/wifi",      HTTP_DELETE, wifi_delete_handler,  NULL },
        { "/wifi",      HTTP_OPTIONS,cors_options_handler, NULL },
        { "/wifi_scan", HTTP_GET,    wifi_scan_handler,    NULL },
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
