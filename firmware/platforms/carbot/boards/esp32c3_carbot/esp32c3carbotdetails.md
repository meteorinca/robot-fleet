# ESP32-C3 CarBot Super Mini

## CarBot v1.0

CarBot is an RC-style car with:
- **L298N motor driver** connected to two DC motors:
  - **Motor A** — Steering (open-loop, no encoder feedback — precise timed control)
  - **Motor B** — Rear-wheel drive (2× motors wired in parallel)
- **0.96" OLED** display for status, IP, ultrasonic readout
- **HC-SR04** ultrasonic distance sensor
- **Passive buzzer** for audio feedback
- **ESP32-C3 Super Mini** (WiFi + OTA built-in)

## Features
- Built-in web server for full drive control
- Intuitive driving UI with throttle + steering sliders, D-pad, keyboard support
- Open-loop steering with timed pulses and estimated position tracking
- GET and POST commands for programmatic/fleet control
- Live ultrasonic readout on webapp and OLED
- Buzzer demos and mini piano
- WiFi provisioning via web UI
- OTA firmware updates