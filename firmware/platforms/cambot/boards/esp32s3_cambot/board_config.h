// boards/esp32s3_cambot/board_config.h
// ============================================================================
//  Board: Seeed Studio XIAO ESP32S3 Sense
//  Chip:  ESP32-S3
//  Notes: WiFi camera RC-car — MJPEG stream over HTTP, OTA updates.
//         Camera module: OV2640 (may be OV3660 on newer revisions) connected
//         Motor driver: L298N dual H-bridge (steering + drive via OUT1-OUT4).
//         via the Sense expansion board's B2B connector.
//
//  Authoritative pin source:
//    https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/
//    Verified against Seeed Studio XIAO ESP32S3 Sense schematic rev 1.1
// ============================================================================
#pragma once

// ── Firmware identity ───────────────────────────────────────────────────────
#define FW_VERSION          "0.1"

// Device number macro helpers to stringify the number passed from CMake
#define _XSTR(x) #x
#define _STR(x) _XSTR(x)

#ifndef DEVICE_NUMBER
#define DEVICE_NUMBER 1
#endif

#define MDNS_HOSTNAME       "cambot" _STR(DEVICE_NUMBER)
#define MDNS_INSTANCE       "CamBot v" _STR(DEVICE_NUMBER)

#define WEB_SERVER_PORT     80
#define DISABLE_OTA         0

// ── Network ─────────────────────────────────────────────────────────────────
#include "secrets.h"           // provides WIFI_SSID_1/2, WIFI_PASS_1/2

// ── Time / NTP ──────────────────────────────────────────────────────────────
#define NTP_SERVER          "pool.ntp.org"
#define TIMEZONE            "CST6CDT,M3.2.0,M11.1.0"   // US Central

// ── Boot button ──────────────────────────────────────────────────────────────
// XIAO / S3 BOOT button is GPIO0
#define BTN_BOOT_GPIO           GPIO_NUM_0

// How long a manual command suppresses the heartbeat (microseconds)
#define LED_MANUAL_OVERRIDE_US  4000000     // 4 s

// ── Camera Pin Configuration ────────────────────────────────────────────────
#if defined(CAMBOT_CAMERA_OV3660) || defined(CAMERA_MODEL_OV3660)

// ── OV3660 Camera Pinout ────────────────────────────────────────────────────
// SIOD (SDA) -> GPIO 4
// SIOC (SCL) -> GPIO 5
// XCLK       -> GPIO 15
// PCLK       -> GPIO 13
// VSYNC      -> GPIO 6
// HREF       -> GPIO 7
// Y9 (D7)    -> GPIO 16
// Y8 (D6)    -> GPIO 17
// Y7 (D5)    -> GPIO 18
// Y6 (D4)    -> GPIO 12
// Y5 (D3)    -> GPIO 10
// Y4 (D2)    -> GPIO 8
// Y3 (D1)    -> GPIO 9
// Y2 (D0)    -> GPIO 11
// PWDN       -> -1 (or GPIO 25)
// RESET      -> -1
// Flash LED  -> GPIO 3

#define CAMERA_PIN_PWDN         -1      // or GPIO 25
#define CAMERA_PIN_RESET        -1
#define CAMERA_PIN_XCLK         15
#define CAMERA_PIN_SIOD         4       // I2C SDA (SCCB)
#define CAMERA_PIN_SIOC         5       // I2C SCL (SCCB)
#define CAMERA_PIN_D7           16      // Y9 (XTAL_32K_N)
#define CAMERA_PIN_D6           17      // Y8
#define CAMERA_PIN_D5           18      // Y7
#define CAMERA_PIN_D4           12      // Y6
#define CAMERA_PIN_D3           10      // Y5
#define CAMERA_PIN_D2           8       // Y4
#define CAMERA_PIN_D1           9       // Y3
#define CAMERA_PIN_D0           11      // Y2
#define CAMERA_PIN_VSYNC        6
#define CAMERA_PIN_HREF         7
#define CAMERA_PIN_PCLK         13

// Onboard flashlight / white LED
#define LED_GPIO                GPIO_NUM_3
#define LED_ACTIVE_LOW          0       // HIGH = ON
#define IS_FLASHLIGHT           1

// Motor driver pins for OV3660 board (avoids GPIO 3 & 4)
#define MOTOR_STEER_IN1         GPIO_NUM_1   // steering channel A+
#define MOTOR_STEER_IN2         GPIO_NUM_2   // steering channel A-
#define MOTOR_DRIVE_IN3         GPIO_NUM_41  // drive channel B+
#define MOTOR_DRIVE_IN4         GPIO_NUM_42  // drive channel B-

#else

// ── Default OV2640 Camera Pinout (XIAO ESP32S3 Sense) ───────────────────────
#define CAMERA_PIN_PWDN         -1      // hardwired on expansion board
#define CAMERA_PIN_RESET        -1      // hardwired on expansion board
#define CAMERA_PIN_XCLK         10
#define CAMERA_PIN_SIOD         40      // I2C SDA (SCCB)
#define CAMERA_PIN_SIOC         39      // I2C SCL (SCCB)
#define CAMERA_PIN_D7           48      // Y9
#define CAMERA_PIN_D6           11      // Y8
#define CAMERA_PIN_D5           12      // Y7
#define CAMERA_PIN_D4           14      // Y6
#define CAMERA_PIN_D3           16      // Y5
#define CAMERA_PIN_D2           18      // Y4
#define CAMERA_PIN_D1           17      // Y3
#define CAMERA_PIN_D0           15      // Y2
#define CAMERA_PIN_VSYNC        38
#define CAMERA_PIN_HREF         47
#define CAMERA_PIN_PCLK         13

// Built-in User LED on XIAO ESP32S3
#define LED_GPIO                GPIO_NUM_21
#define LED_ACTIVE_LOW          0       // HIGH = ON
#define IS_FLASHLIGHT           0

// Motor mapping (XIAO Sense D0-D3 / GPIO 1-4)
#define MOTOR_STEER_IN1         GPIO_NUM_1   // green  — steering forward/left
#define MOTOR_STEER_IN2         GPIO_NUM_2   // blue   — steering reverse/right
#define MOTOR_DRIVE_IN3         GPIO_NUM_3   // purple — drive forward
#define MOTOR_DRIVE_IN4         GPIO_NUM_4   // grey   — drive reverse

#endif

// Camera clock: 20MHz standard for OV2640 and OV3660
#define CAMERA_XCLK_FREQ_HZ     20000000

// Default resolution: VGA (640×480) — fast 25-30 FPS, crisp image
#define CAMERA_FRAME_SIZE       FRAMESIZE_VGA

// JPEG quality: 16 (prevents DMA frame buffer overflow FB-OVF on OV2640 & OV3660)
#define CAMERA_JPEG_QUALITY     16

// Frame buffers in PSRAM (requires CONFIG_SPIRAM=y)
#define CAMERA_FB_COUNT         2

// ── Scheduler ─────────────────────────────────────────────────────────────────
#define MAX_SCHEDULED_ACTIONS  8
