// boards/esp32c3_rfbot315/board_config.h
// ============================================================================
//  Board: ESP32-C3 RFBot (315 MHz)
//  Chip:  ESP32-C3
//  Notes: RF bot (315 MHz variant) with built-in LED, OTA, RF receiver/transmitter and two servos
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

// This hostname sets the base mDNS name.
// E.g. "rfbot315" -> "rfbot315_5.local"
#define MDNS_HOSTNAME       "rfbot315_" _STR(DEVICE_NUMBER)
#define MDNS_INSTANCE       "RFBot 315MHz v" _STR(DEVICE_NUMBER)

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

// Heartbeat colours
#define WS2812_COLOR_CONNECTED  0x000A00    // dim green
#define WS2812_COLOR_DISCO      0x0A0000    // dim red
#define WS2812_COLOR_ACTION     0x00000A    // dim blue

// How long a manual command suppresses the heartbeat (microseconds)
#define LED_MANUAL_OVERRIDE_US  4000000     // 4 s

// ── Servos (2 servos for rf bot) ────────────────────────────────────────
#define SERVO_COUNT             2
#define SERVO1_GPIO             GPIO_NUM_5
#define SERVO2_GPIO             GPIO_NUM_6

#define LEDC_CH_SERVO1          LEDC_CHANNEL_0
#define LEDC_CH_SERVO2          LEDC_CHANNEL_1

#define LEDC_TIMER              LEDC_TIMER_0
#define SERVO_MIN_PULSE_US      500
#define SERVO_MAX_PULSE_US      2500
#define SERVO_RETURN_MS         1000

#define POS1_ON                 171
#define POS1_NEUTRAL            121
#define POS1_OFF                70

#define POS2_ON                 171
#define POS2_NEUTRAL            121
#define POS2_OFF                70

// ── Buttons ──────────────────────────────────────────────────────────────────
#define BTN_BOOT_GPIO           GPIO_NUM_9    // boot button
#define BTN_1_GPIO              GPIO_NUM_0    // user button 1
#define BTN_2_GPIO              GPIO_NUM_1    // user button 2

// ── 315 MHz RF Module ───────────────────────────────────────────────────────
#define RF_FREQ_MHZ             315
// TX and RX GPIOs for the 315 MHz transmitter / receiver
#define RF_TX_GPIO              GPIO_NUM_3
#define RF_RX_GPIO              GPIO_NUM_10
#define RF_PULSE_WIDTH          185

// Hard-coded RF action codes (used for physical remote dispatch)
#define RF_CODE_TOGGLE_LED      0x123456
#define RF_CODE_SERVO1          0x789ABC

// ── Scheduler ─────────────────────────────────────────────────────────────────
#define MAX_SCHEDULED_ACTIONS  8
