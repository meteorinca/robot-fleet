// boards/esp32c3_dog/board_config.h
// ============================================================================
//  Board: ESP32-C3 Quadruped Dog Robot
//  Chip:  ESP32-C3  (NO touch sensor, NO RF module)
//  Notes: 4 leg servos, OLED display, LED strip, audio PDM, wake buttons
// ============================================================================
#pragma once

// ── Firmware identity ───────────────────────────────────────────────────────
#define FW_VERSION          "0.3"

// Device number macro helpers to stringify the number passed from CMake
#define _XSTR(x) #x
#define _STR(x) _XSTR(x)

#ifndef DEVICE_NUMBER
#define DEVICE_NUMBER 2
#endif

#define MDNS_HOSTNAME       "dogbot" _STR(DEVICE_NUMBER)
#define MDNS_INSTANCE       "MOJDogv" _STR(DEVICE_NUMBER)

#define WEB_SERVER_PORT     81
#define DISABLE_OTA         0

// ── Network ─────────────────────────────────────────────────────────────────
#include "secrets.h"           // provides WIFI_SSID, WIFI_PASS

// ── Time / NTP ──────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define TIMEZONE            "CST6CDT,M3.2.0,M11.1.0"   // US Central

// ── LED (GPIO 8, simple GPIO/LEDC) ──────────────────────────────────────────
#define LED_GPIO                GPIO_NUM_8
#define LED_ACTIVE_LOW          1       // HIGH = OFF

// ── WS2812 NeoPixel strip (DISABLED) ────────────────────────────────────────
// #define WS2812_GPIO             GPIO_NUM_8
// #define WS2812_NUM_LEDS         4

// Heartbeat colours (legacy for code that uses these names, but simple LED is single color)
#define WS2812_COLOR_CONNECTED  0x000A00    // dim green
#define WS2812_COLOR_DISCO      0x0A0000    // dim red
#define WS2812_COLOR_ACTION     0x00000A    // dim blue

// How long a manual command suppresses the heartbeat (microseconds)
#define LED_MANUAL_OVERRIDE_US  4000000     // 4 s

// ── Servos (4 leg servos) ────────────────────────────────────────────────────
#define SERVO_COUNT         4

// Front-Left
#define SERVO1_GPIO         GPIO_NUM_21   // FL leg
#define LEDC_CH_SERVO1      LEDC_CHANNEL_0
#define POS1_ON             0
#define POS1_NEUTRAL        90
#define POS1_OFF            120

// Front-Right
#define SERVO2_GPIO         GPIO_NUM_19   // FR leg
#define LEDC_CH_SERVO2      LEDC_CHANNEL_1
#define POS2_ON             0
#define POS2_NEUTRAL        90
#define POS2_OFF            120

// Back-Left
#define SERVO3_GPIO         GPIO_NUM_20   // BL leg
#define LEDC_CH_SERVO3      LEDC_CHANNEL_2
#define POS3_ON             0
#define POS3_NEUTRAL        90
#define POS3_OFF            120

// Back-Right
#define SERVO4_GPIO         GPIO_NUM_18   // BR leg
#define LEDC_CH_SERVO4      LEDC_CHANNEL_3
#define POS4_ON             0
#define POS4_NEUTRAL        90  
#define POS4_OFF            120

#define LEDC_TIMER          LEDC_TIMER_0
#define SERVO_MIN_PULSE_US  500
#define SERVO_MAX_PULSE_US  2500
#define SERVO_RETURN_MS     500

// ── Display (SPI OLED/LCD — 160x80) ─────────────────────────────────────────
#define DISP_MOSI_GPIO      GPIO_NUM_4
#define DISP_CLK_GPIO       GPIO_NUM_5
#define DISP_DC_GPIO        GPIO_NUM_10
#define DISP_RST_GPIO       GPIO_NUM_NC   // not connected
#define DISP_CS_GPIO        GPIO_NUM_NC   // not connected / tied GND

// ── Audio (PDM speaker) ──────────────────────────────────────────────────────
#define AUDIO_DATA_GPIO     GPIO_NUM_6
#define AUDIO_CLK_GPIO      GPIO_NUM_7
#define AUDIO_AMP_GPIO      GPIO_NUM_3    // amp enable

// ── Buttons ──────────────────────────────────────────────────────────────────
#define BTN_BOOT_GPIO       GPIO_NUM_9    // boot / menu button
#define BTN_MOVE_WAKE_GPIO  GPIO_NUM_0    // wake-on-movement
#define BTN_AUDIO_WAKE_GPIO GPIO_NUM_1    // wake-on-audio

// ── RF ────────────────────────────────────────────────────────────────────────
//  C3 dog has NO 433 MHz module. rf.c is excluded from the build by CMakeLists.
//  Leave RF_RX/TX undefined so any accidental include generates a clear error.

// ── Touch pads ────────────────────────────────────────────────────────────────
//  ESP32-C3 has NO touch sensor hardware.
//  touch_input.c is compiled but the SOC_TOUCH_SENSOR_SUPPORTED guard makes
//  it a no-op, and touch_input_start() is never called from main.c.

// ── Scheduler ─────────────────────────────────────────────────────────────────
#define MAX_SCHEDULED_ACTIONS  8
