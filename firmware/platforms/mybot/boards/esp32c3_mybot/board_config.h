// boards/esp32c3_mybot/board_config.h
// ============================================================================
//  Board: ESP32-C3 MyBot
//  Chip:  ESP32-C3
//  Notes: MyBot with a built-in LED, ultrasonic sr04, neopixels, 1 continuous rotation servo, webUI and OTA capabilities
// ============================================================================
#pragma once

// ── Firmware identity ───────────────────────────────────────────────────────
#define FW_VERSION          "1.0"

// Device number macro helpers to stringify the number passed from CMake
#define _XSTR(x) #x
#define _STR(x) _XSTR(x)

#ifndef DEVICE_NUMBER
#define DEVICE_NUMBER 5
#endif

#define MDNS_HOSTNAME       "mybot" _STR(DEVICE_NUMBER)
#define MDNS_INSTANCE       "MyBot v" _STR(DEVICE_NUMBER)

#define WEB_SERVER_PORT     80
#define DISABLE_OTA         0

// ── Network ─────────────────────────────────────────────────────────────────
#include "secrets.h"           // provides WIFI_SSID, WIFI_PASS

// ── Time / NTP ──────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define TIMEZONE            "CST6CDT,M3.2.0,M11.1.0"   // US Central

// ── LED (Built-in GPIO LED) ─────────────────────────────────────────────────
// Adjust LED_GPIO for your specific board. Common values: 8 (C3 SuperMini), 2 (generic)
#define LED_GPIO                GPIO_NUM_8
#define LED_ACTIVE_LOW          1       // HIGH = OFF
// ── NeoPixel Strip (WS2812) ─────────────────────────────────────────────────
// Connects to GPIO 3 by default. Change if you wire the strip elsewhere.
// NUM_LEDS = number of pixels in the strip shipped with the kit.
#define WS2812_GPIO             GPIO_NUM_3
#define WS2812_NUM_LEDS         10
#define WS2812_RMT_RES_HZ       10000000  // 10MHz RMT resolution
// Heartbeat colours (legacy for code that uses these names, but simple LED is single color)
#define WS2812_COLOR_CONNECTED  0x000A00    // dim green
#define WS2812_COLOR_DISCO      0x0A0000    // dim red
#define WS2812_COLOR_ACTION     0x00000A    // dim blue

// How long a manual command suppresses the heartbeat (microseconds)
#define LED_MANUAL_OVERRIDE_US  4000000     // 4 s

// ── Servos (1 continuous rotation servo for mybot) ────────────────────────────────────────
#define SERVO_COUNT             1
#define SERVO1_GPIO             GPIO_NUM_5
#define LEDC_CH_SERVO1          LEDC_CHANNEL_0
#define LEDC_TIMER              LEDC_TIMER_0
#define SERVO_MIN_PULSE_US      500
#define SERVO_MAX_PULSE_US      2500
#define SERVO_RETURN_MS         1000

#define POS1_ON                 171
#define POS1_NEUTRAL            121
#define POS1_OFF                70

// ── Buttons ──────────────────────────────────────────────────────────────────
#define BTN_BOOT_GPIO           GPIO_NUM_9    // boot button
#define BTN_1_GPIO              GPIO_NUM_0    // user button 1
#define BTN_2_GPIO              GPIO_NUM_1    // user button 2

// HC-SR04 Ultrasonic Distance Sensor
#define TRIG_PIN            GPIO_NUM_10
#define ECHO_PIN            GPIO_NUM_4

// ── Scheduler ─────────────────────────────────────────────────────────────────
#define MAX_SCHEDULED_ACTIONS  8

