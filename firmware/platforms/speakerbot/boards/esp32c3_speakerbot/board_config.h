// boards/esp32c3_speakerbot/board_config.h
// ============================================================================
//  Board: ESP32-C3 SpeakerBot
//  Chip:  ESP32-C3
//  Notes: MyBot with all features (OLED, Servo, Ultrasonic, Buzzer, LEDs,
//         WiFi hotspot, OTA) PLUS I2S PDM speaker & streaming audio support.
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

#define MDNS_HOSTNAME       "speakerbot" _STR(DEVICE_NUMBER)
#define MDNS_INSTANCE       "SpeakerBot v" _STR(DEVICE_NUMBER)

#define WEB_SERVER_PORT     80
#define DISABLE_OTA         0

// ── Network ─────────────────────────────────────────────────────────────────
#include "secrets.h"           // provides WIFI_SSID_1/2, WIFI_PASS_1/2

// ── Time / NTP ──────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define TIMEZONE            "CST6CDT,M3.2.0,M11.1.0"   // US Central

// ── LED (Built-in GPIO LED) ─────────────────────────────────────────────────
#define LED_GPIO                GPIO_NUM_8
#define LED_ACTIVE_LOW          1       // HIGH = OFF

// How long a manual command suppresses the heartbeat (microseconds)
#define LED_MANUAL_OVERRIDE_US  4000000     // 4 s

// ── Buzzer (Passive) ──────────────────────────────────────────────────────────
// Connected to GPIO 3
#define BUZZER_PIN              GPIO_NUM_3

// ── Servos (1 sg90 servo) ────────────────────────────────────────────────────
#define SERVO_COUNT             1
#define SERVO1_GPIO             GPIO_NUM_5
#define LEDC_CH_SERVO1          LEDC_CHANNEL_0
#define LEDC_TIMER              LEDC_TIMER_0
#define SERVO_MIN_PULSE_US      500
#define SERVO_MAX_PULSE_US      2500
#define SERVO_RETURN_MS         1000

#define POS1_ON                 171
#define POS1_NEUTRAL            90
#define POS1_OFF                70

// ── OLED Display (I2C SSD1306 128x64) ───────────────────────────────────────
#define OLED_SDA_PIN        7
#define OLED_SCL_PIN        6
#define OLED_ADDR           0x3C

#if defined(ENABLE_ULTRASONIC) && ENABLE_ULTRASONIC
// ── HC-SR04 Ultrasonic Distance Sensor ──────────────────────────────────────
#define TRIG_PIN            GPIO_NUM_10
#define ECHO_PIN            GPIO_NUM_4
#endif


// ── LEDs and Buttons from controller breadboard ──────────────────────────────
#define LED_GRN_PIN         GPIO_NUM_20
#define LED_RED_PIN         GPIO_NUM_21
#define BTN_1_GPIO          GPIO_NUM_0
#define BTN_2_GPIO          GPIO_NUM_1
#define BTN_BOOT_GPIO       GPIO_NUM_9    // boot button

// ── Audio (I2S PDM Speaker) ─────────────────────────────────────────────────
// PDM microphone/speaker on I2S bus
#define AUDIO_DATA_GPIO     GPIO_NUM_1    // I2S PDM TX Data — NOTE: shared breadboard btn, wire to standalone speaker module
#define AUDIO_CLK_GPIO      GPIO_NUM_2    // I2S PDM TX Clock
#define AUDIO_AMP_GPIO      GPIO_NUM_18   // Amplifier enable (HIGH = on)

// ── WiFi provisioning ────────────────────────────────────────────────────────
#define AP_FALLBACK_TIMEOUT_MS  10000   // ms before SoftAP fallback (per SSID)
#define MAX_NVS_NETWORKS        8       // max user-saved WiFi networks in NVS
#define ENABLE_CAPTIVE_PORTAL   1       // DNS redirect to 192.168.4.1 in AP mode

// ── Scheduler ─────────────────────────────────────────────────────────────────
#define MAX_SCHEDULED_ACTIONS  8
