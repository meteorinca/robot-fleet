#include "webserver.h"
#include "config.h"
#include "led.h"
// WS2812 NeoPixel driver — only compiled when the strip is configured
#ifdef WS2812_NUM_LEDS
#include "ws2812.h"
#endif
#include "timekeep.h"
#include "ota_mgr.h"
#include "servo.h"
#include "ultrasonic.h"
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

    if      (strcmp(action, "s1on")   == 0) servo_action_set(1, servo_get_last_angle(1));
    else if (strcmp(action, "s1off")  == 0) servo_detach(1);
    else if (strcmp(action, "us_on")  == 0) ultrasonic_set_active(true);
    else if (strcmp(action, "us_off") == 0) ultrasonic_set_active(false);
    else if (strcmp(action, "l1on")   == 0) led_action_set(true);
    else if (strcmp(action, "l1off")  == 0) led_action_set(false);
    else if (strcmp(action, "toggle") == 0) led_action_toggle();
    else if (strcmp(action, "hi")     == 0) servo_quick_action(1, 40, POS1_NEUTRAL);
    // tts:<text> — push text to SSE clients for browser-side synthesis
    else if (strncmp(action, "tts:", 4) == 0) sse_broadcast_tts(action + 4);
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
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang='en'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<meta name='apple-mobile-web-app-capable' content='yes'>"
        "<title>Moe's MyBot Control</title>"
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
        "<h1 style='font-size:28px;font-weight:800;color:#e0e0f0;text-align:center;margin:0 0 16px 0;letter-spacing:1px;'>Moe's MyBot V1.1</h1>"
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
        "<h2>Continuous Servo</h2>"
        "<div style='margin-bottom:15px;'>"
        "  <div style='display:flex;justify-content:space-between;margin-bottom:5px;'>"
        "    <label>Speed (Reverse - Stop - Forward)</label>"
        "    <span id='s1-val'>90</span>"
        "  </div>"
        "  <input type='range' min='0' max='180' value='90' class='slider' id='s1-slide' oninput='sv(1,this.value)'>"
        "  <div class='btn-led-grid' style='margin-top:5px;'>"
        "    <button class='btn-led' onclick='dog(\"s1on\")'>ON</button>"
        "    <button class='btn-led' onclick='dog(\"s1off\")'>OFF</button>"
        "  </div>"
        "</div>"
        "</div>"

        "<div class='card'>"
        "<h2>Ultrasonic Sensor</h2>"
        "<div class='btn-led-grid' style='grid-template-columns:1fr 1fr;'>"
        "  <button class='btn-led' onclick='dog(\"us_on\")' style='color:#00e5a0;border-color:#00e5a0;'>ON</button>"
        "  <button class='btn-led' onclick='dog(\"us_off\")' style='color:#f7736a;border-color:#f7736a;'>OFF</button>"
        "</div>"
        "<div style='margin-top:15px;text-align:center;'>"
        "  <div class='time-big' id='us-val'>-- cm</div>"
        "  <div style='background:#0a0a1e;border-radius:6px;height:8px;margin-top:8px;overflow:hidden;'>"
        "    <div id='us-bar' style='width:0%;height:100%;background:linear-gradient(90deg,#00e5a0,#7c6af7);transition:width .1s;'></div>"
        "  </div>"
        "</div>"
        "</div>"

        "<div class='card'>"
        "<h2>NeoPixel Animations</h2>"
        "<div class='btn-led-grid' style='grid-template-columns:1fr 1fr 1fr;'>"
        "<button class='btn-led' onclick='np_rbw()' style='color:#ff55ff;border-color:#ff55ff;'>RAINBOW</button>"
        "<button class='btn-led' onclick='np_plc()' style='color:#5555ff;border-color:#5555ff;'>POLICE</button>"
        "<button class='btn-led' onclick='np_dsc()' style='color:#55ff55;border-color:#55ff55;'>DISCO</button>"
        "<button class='btn-led' onclick='np_pac()' style='color:#55ffff;border-color:#55ffff;'>PACIFICA</button>"
        "<button class='btn-led' onclick='np_on()' style='color:#ffffff;border-color:#ffffff;'>ON</button>"
        "<button class='btn-led' onclick='np_clr()' style='color:#a0a0a0;'>OFF</button>"
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
        "<h2>Actions</h2>"
        "<div class='action-grid' id='action-grid'></div>"
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
        "function led(a){fetch('/'+a);}"
        "function dog(v){fetch('/'+v);}"
        "function sv(n,a){"
        "document.getElementById('s'+n+'-val').textContent=a;"
        "fetch('/s'+n+'_'+a);"
        "}"
        "var npT=0;function np_clr(){clearInterval(npT);fetch('/neopixel_clear');}"
        "function np_on(){clearInterval(npT);fetch('/neopixel_all?r=255&g=255&b=255');}"
        "function np_pac(){clearInterval(npT);fetch('/neopixel_pacifica');}"
        "function np_rbw(){clearInterval(npT);var h=0;npT=setInterval(function(){"
        "h=(h+15)%360;var f=function(n){var k=(n+h/60)%6;return 255-Math.round(255*Math.max(0,Math.min(k,4-k,1)));};"
        "fetch('/neopixel_all?r='+f(5)+'&g='+f(3)+'&b='+f(1));},200);}"
        "function np_plc(){clearInterval(npT);var t=0;npT=setInterval(function(){"
        "t=!t;if(t)fetch('/neopixel_all?r=255&g=0&b=0');else fetch('/neopixel_all?r=0&g=0&b=255');},250);}"
        "function np_dsc(){clearInterval(npT);npT=setInterval(function(){"
        "fetch('/neopixel_all?r='+Math.floor(Math.random()*255)+'&g='+Math.floor(Math.random()*255)+'&b='+Math.floor(Math.random()*255));},150);}"
        "function scheduleAction(){"
        "var d=document.getElementById('sched-delay').value;"
        "var a=document.getElementById('sched-action').value;"
        "fetch('/schedule?action='+a+'&delay='+d).then(function(r){return r.json();})"
        ".then(function(j){ document.getElementById('sched-err').textContent='Scheduled '+a+' in '+d+'s'; })"
        ".catch(function(e){ document.getElementById('sched-err').textContent=e; });"
        "}"
        "var ACTIONS={'hi':'Say Hi','s1on':'S1 ON','s1off':'S1 OFF','us_on':'US ON','us_off':'US OFF'};"
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
        "es.onmessage=function(e){"
        "try { var j=JSON.parse(e.data); if(j.type==='us'){"
        "document.getElementById('us-val').textContent=j.dist+' cm';"
        "var pct=Math.min(100, Math.max(0, ((50-j.dist)/50)*100));"
        "document.getElementById('us-bar').style.width=pct+'%';"
        "} }catch(ex){}"
        "};"
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
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
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

#ifdef WS2812_NUM_LEDS
// ══════════════════════════════════════════════════════════════
//  NeoPixel endpoints  (only when WS2812_NUM_LEDS is defined)
// ══════════════════════════════════════════════════════════════

// GET /neopixel?pixel=N&r=R&g=G&b=B
static esp_err_t neopixel_handler(httpd_req_t *req) {
    char buf[64];
    int pixel = 0, r = 0, g = 0, b = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[8];
        if (httpd_query_key_value(buf, "pixel", p, sizeof(p)) == ESP_OK) pixel = atoi(p);
        if (httpd_query_key_value(buf, "r",     p, sizeof(p)) == ESP_OK) r     = atoi(p);
        if (httpd_query_key_value(buf, "g",     p, sizeof(p)) == ESP_OK) g     = atoi(p);
        if (httpd_query_key_value(buf, "b",     p, sizeof(p)) == ESP_OK) b     = atoi(p);
    }
    // Clamp values
    if (pixel < 0) pixel = 0;
    if (pixel >= WS2812_NUM_LEDS) pixel = WS2812_NUM_LEDS - 1;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;

    ws2812_set_pixel(pixel, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    ws2812_show();
    ESP_LOGI("WEB", "NeoPixel: pixel=%d r=%d g=%d b=%d", pixel, r, g, b);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /neopixel_all?r=R&g=G&b=B
static esp_err_t neopixel_all_handler(httpd_req_t *req) {
    char buf[48];
    int r = 0, g = 0, b = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[8];
        if (httpd_query_key_value(buf, "r", p, sizeof(p)) == ESP_OK) r = atoi(p);
        if (httpd_query_key_value(buf, "g", p, sizeof(p)) == ESP_OK) g = atoi(p);
        if (httpd_query_key_value(buf, "b", p, sizeof(p)) == ESP_OK) b = atoi(p);
    }
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;

    uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    ws2812_set_all(rgb);
    ws2812_show();
    ESP_LOGI("WEB", "NeoPixel all: r=%d g=%d b=%d", r, g, b);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /neopixel_clear
static esp_err_t neopixel_clear_handler(httpd_req_t *req) {
    ws2812_clear();
    ws2812_show();
    ESP_LOGI("WEB", "NeoPixel cleared");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /neopixel_pacifica
static esp_err_t neopixel_pacifica_handler(httpd_req_t *req) {
    ws2812_resume_heartbeat();
    ESP_LOGI("WEB", "NeoPixel pacifica resumed");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif // WS2812_NUM_LEDS

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

    sse_init();

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
        { "/events",    HTTP_GET,  sse_handler,            NULL },
        { "/sync_time", HTTP_GET,  sync_time_handler,      NULL },
        // ... (rest of quick actions)
        { "/l1on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/l1off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/toggle",    HTTP_GET,  quick_action_handler,   NULL },
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

#ifdef DISP_MOSI_GPIO
        { "/audio",     HTTP_POST, audio_post_handler,     NULL },
        { "/audio",     HTTP_OPTIONS, cors_options_handler,NULL },
#endif
        { "/ota",       HTTP_POST, ota_post_handler,       NULL },
        { "/ota",       HTTP_OPTIONS, cors_options_handler,NULL },
#ifdef WS2812_NUM_LEDS
        { "/neopixel",       HTTP_GET, neopixel_handler,       NULL },
        { "/neopixel_all",   HTTP_GET, neopixel_all_handler,   NULL },
        { "/neopixel_clear", HTTP_GET, neopixel_clear_handler, NULL },
        { "/neopixel_pacifica", HTTP_GET, neopixel_pacifica_handler, NULL },
#endif
        { "/servo",     HTTP_GET,  servo_handler,          NULL },
        { "/s1_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
        { "/s2_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
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
