import re

path = 'c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/cambot/cambot/main/webserver.c'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# We need to replace from the mangled #cam-ph line to the end of .layout
# Let's find the start of the mangled line
start_marker = '        "#cam-img{width:100%;height:100%;object-fit:contain;display:none;}"\n'
start_idx = content.find(start_marker) + len(start_marker)

end_marker = '        "</div>" /* end .layout */\n'
end_idx = content.find(end_marker, start_idx) + len(end_marker)

proper_content = """        "#cam-ph{display:flex;flex-direction:column;align-items:center;gap:10px;color:#282840;}"
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
        ".joy-panel{width:220px;flex-shrink:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:18px;"
        "background:linear-gradient(160deg,#0c0c1e,#0a0a18);border-left:1px solid #151528;padding:16px 12px;}"
        ".joy-label{font-size:9px;font-weight:700;letter-spacing:1.5px;color:#333350;text-transform:uppercase;}"
        /* Joystick circle */
        ".joy-outer{position:relative;width:160px;height:160px;border-radius:50%;"
        "background:radial-gradient(circle at 50% 50%,#0f0f22,#080814);"
        "border:2px solid #1e1e38;box-shadow:0 0 24px rgba(124,106,247,.1);}"
        ".joy-ring{position:absolute;inset:8px;border-radius:50%;border:1px solid #1e1e38;}"
        ".joy-cross-h{position:absolute;top:50%;left:0;right:0;height:1px;background:#151528;transform:translateY(-50%);}"
        ".joy-cross-v{position:absolute;left:50%;top:0;bottom:0;width:1px;background:#151528;transform:translateX(-50%);}"
        ".joy-knob{position:absolute;width:56px;height:56px;border-radius:50%;"
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
        ".joy-panel{width:100%;flex-direction:row;flex-wrap:wrap;border-left:none;border-top:1px solid #151528;padding:12px;justify-content:space-around;gap:12px;}"
        ".joy-outer{width:120px;height:120px;}"
        ".joy-knob{width:44px;height:44px;}"
        "}"
        "</style>"
        "</head><body>"
        
        /* OTA drawer */
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

        /* Right column - joystick */
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

        "<div class='speed-wrap' style='width:100%; padding-top:10px;'>"
        "<div class='joy-label' style='margin-bottom:8px'>Max Speed: <span id='spd-val'>50</span>%</div>"
        "<input type='range' id='spd-slider' min='10' max='100' value='50' style='width:100%; cursor:pointer;' oninput='document.getElementById(\"spd-val\").textContent=this.value; maxSpeed=this.value/100;'>"
        "</div>"

        "<div class='wasd-hint'>WASD / Arrow keys &middot; Gamepad supported</div>"
        "</div>"

        "</div>" /* end .layout */
"""

new_content = content[:start_idx] + proper_content + content[end_idx:]

with open(path, 'w', encoding='utf-8') as f:
    f.write(new_content)

print("Done")
