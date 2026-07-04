# CarBot Wiring (ESP32-C3 Super Mini)

## L298N Motor Driver

### Motor A — Steering (open-loop, no encoder)
| L298N Pin | ESP32-C3 Pin | Notes |
|-----------|--------------|-------|
| ENA       | 3.3V         | Always enabled (PWM via IN pins) |
| IN1       | GPIO 5       | Steer RIGHT (PWM via LEDC CH2) |
| IN2       | GPIO 20      | Steer LEFT  (PWM via LEDC CH5) |
| OUT1/OUT2 | Steering motor | |

### Motor B — Drive (RWD, 2× motors in parallel)
| L298N Pin | ESP32-C3 Pin | Notes |
|-----------|--------------|-------|
| ENB       | 3.3V         | Always enabled (PWM via IN pins) |
| IN3       | GPIO 1       | Drive FORWARD (PWM via LEDC CH3) |
| IN4       | GPIO 0       | Drive REVERSE (PWM via LEDC CH4) |
| OUT3/OUT4 | Drive motors | Two motors wired in parallel |

### Power
| Pin | Connection |
|-----|------------|
| 12V | LiPo / power bank |
| GND | ESP32 GND + common ground |
| 5V  | ESP32 5V (optional, use separate regulator) |

---

## Passive Buzzer
| Pin    | ESP32-C3 Pin |
|--------|--------------|
| Signal | GPIO 3       |
| GND    | GND          |

## 0.96" OLED (I2C)
| Pin | ESP32-C3 Pin |
|-----|--------------|
| VCC | 3.3V         |
| SDA | GPIO 7       |
| SCL | GPIO 6       |
| GND | GND          |

## HC-SR04 Ultrasonic
| Pin  | ESP32-C3 Pin |
|------|--------------|
| VCC  | 5V           |
| Trig | GPIO 10      |
| Echo | GPIO 4       |
| GND  | GND          |

## Built-in LED
- GPIO 8 (active LOW — HIGH = OFF)

## Boot Button
- GPIO 9 (pulled up, active LOW)
- Short press: Show IP on OLED
- Hold 7s: WiFi reset confirmation

---

## ⚠️ Pin Conflicts
GPIO 0, 1, and 20 are shared between the motor driver and the original breadboard LED/Button pins from mybot.
**Do NOT wire external LEDs or buttons to GPIO 0, 1, or 20 when using the L298N.**