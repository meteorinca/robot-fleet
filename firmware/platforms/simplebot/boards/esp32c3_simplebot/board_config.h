// boards/esp32c3_simplebot/board_config.h
// ============================================================================
//  Board: ESP32-C3 Simple Bot
//  Chip:  ESP32-C3
//  Notes: Simple bot with only a built-in LED and OTA capabilities.
// ============================================================================
#pragma once

// ── Firmware identity ───────────────────────────────────────────────────────
#define FW_VERSION          "0.1"

// Device number macro helpers to stringify the number passed from CMake
#define _XSTR(x) #x
#define _STR(x) _XSTR(x)

#ifndef DEVICE_NUMBER
#define DEVICE_NUMBER 5
#endif

#define MDNS_HOSTNAME       "simplebot" _STR(DEVICE_NUMBER)
#define MDNS_INSTANCE       "SimpleBot v" _STR(DEVICE_NUMBER)

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

// Heartbeat colours (legacy for code that uses these names, but simple LED is single color)
#define WS2812_COLOR_CONNECTED  0x000A00    // dim green
#define WS2812_COLOR_DISCO      0x0A0000    // dim red
#define WS2812_COLOR_ACTION     0x00000A    // dim blue

// How long a manual command suppresses the heartbeat (microseconds)
#define LED_MANUAL_OVERRIDE_US  4000000     // 4 s

// Stub for servo_count to allow simple compilation if other headers reference it
static inline int servo_count() { return 0; }

// ── Scheduler ─────────────────────────────────────────────────────────────────
#define MAX_SCHEDULED_ACTIONS  8

