// boards/esp32c3_carbot/board_config.h
// ============================================================================
//  Board: ESP32-C3 CarBot Super Mini
//  Chip:  ESP32-C3
//  Notes: CarBot with L298N dual motor driver.
//         Motor A (ENA) = Steering motor (no encoder feedback)
//         Motor B (ENB) = Rear-wheel drive (2x RWD motors in parallel)
//         Also has OLED display, HC-SR04 ultrasonic, passive buzzer, OTA.
// ============================================================================
#pragma once

// ── Firmware identity ───────────────────────────────────────────────────────
#define FW_VERSION          "v1.0"

// Device number macro helpers to stringify the number passed from CMake
#define _XSTR(x) #x
#define _STR(x) _XSTR(x)

#ifndef DEVICE_NUMBER
#define DEVICE_NUMBER 1
#endif

#define MDNS_HOSTNAME       "carbot" _STR(DEVICE_NUMBER)
#define MDNS_INSTANCE       "CarBot v" _STR(DEVICE_NUMBER)

#define WEB_SERVER_PORT     80
#define DISABLE_OTA         0

// ── Network ─────────────────────────────────────────────────────────────────
#include "secrets.h"           // provides WIFI_SSID, WIFI_PASS

// ── Time / NTP ──────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define TIMEZONE            "CST6CDT,M3.2.0,M11.1.0"   // US Central

// ── LED (Built-in GPIO LED) ─────────────────────────────────────────────────
#define LED_GPIO                GPIO_NUM_8
#define LED_ACTIVE_LOW          1       // HIGH = OFF

// ── Buzzer (Passive) ──────────────────────────────────────────────────────────
#define BUZZER_PIN              GPIO_NUM_3

// How long a manual command suppresses the heartbeat (microseconds)
#define LED_MANUAL_OVERRIDE_US  4000000     // 4 s

// ── L298N Motor Driver ───────────────────────────────────────────────────────
//  Motor A  = Steering (no encoder, open-loop timed control)
//    ENA    → 3.3V (always enabled — PWM speed done via IN1/IN2 duty)
//    IN1    → GPIO 5
//    IN2    → GPIO 20
//  Motor B  = Drive (RWD, two motors wired in parallel)
//    ENB    → 3.3V (always enabled)
//    IN3    → GPIO 1
//    IN4    → GPIO 0
//
// LEDC channels used for PWM:
//   LEDC_CHANNEL_0  → STEER_ENA (if we ever add EN PWM — optional)
//   LEDC_CHANNEL_1  → DRIVE_ENA (if we ever add EN PWM — optional)
//
// Direction is controlled via IN1/IN2 and IN3/IN4 GPIO levels.
// Speed is controlled by duty-cycling IN1 or IN2 (the "active" pin).

#define MOTOR_STEER_IN1     GPIO_NUM_5
#define MOTOR_STEER_IN2     GPIO_NUM_20

#define MOTOR_DRIVE_IN3     GPIO_NUM_1
#define MOTOR_DRIVE_IN4     GPIO_NUM_0

// LEDC for PWM speed control (steering + drive)
#define MOTOR_LEDC_TIMER        LEDC_TIMER_1
#define MOTOR_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define MOTOR_LEDC_FREQ_HZ      5000
#define MOTOR_LEDC_RESOLUTION   LEDC_TIMER_10_BIT   // 0-1023

#define MOTOR_LEDC_CH_STEER     LEDC_CHANNEL_2   // drives STEER_IN1 for fwd PWM
#define MOTOR_LEDC_CH_DRIVE     LEDC_CHANNEL_3   // drives DRIVE_IN3 for fwd PWM

// Steering calibration — open-loop timed pulses for precision
// Full-left: angle 0  →  Full-right: angle 100  →  Center: 50
#define STEER_FULL_TURN_MS      800     // ms to go from center to hard lock
#define STEER_DEADBAND          3       // ignore changes smaller than this (0-100 scale)

// ── OLED Display (I2C) ───────────────────────────────────────────────────────
#define OLED_SDA_PIN        7
#define OLED_SCL_PIN        6
#define OLED_ADDR           0x3C

// HC-SR04 Ultrasonic Distance Sensor
#define TRIG_PIN            GPIO_NUM_10
#define ECHO_PIN            GPIO_NUM_4

// External LEDs and Buttons from controller breadboard
#define LED_GRN_PIN         GPIO_NUM_20   // NOTE: shared with MOTOR_STEER_IN2 — disable in motor mode
#define LED_RED_PIN         GPIO_NUM_21
#define BTN_1_GPIO          GPIO_NUM_0    // NOTE: shared with MOTOR_DRIVE_IN4 — disable in motor mode
#define BTN_2_GPIO          GPIO_NUM_1    // NOTE: shared with MOTOR_DRIVE_IN3 — disable in motor mode
#define BTN_BOOT_GPIO       GPIO_NUM_9    // boot button

// ── Scheduler ─────────────────────────────────────────────────────────────────
#define MAX_SCHEDULED_ACTIONS  8
