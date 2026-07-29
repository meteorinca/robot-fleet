// main/config.h
// ============================================================================
//  MOJ ESP32 Template — Board Router
// ============================================================================
#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/soc_caps.h"

// ── Active board selection (injected by CMakeLists.txt via -DBOARD_CONFIG_HEADER=) ─
#ifdef BOARD_CONFIG_HEADER
  #include BOARD_CONFIG_HEADER         // e.g. "../boards/esp32c3_speakerbot/board_config.h"
#else
  #error "No BOARD selected. Run: idf.py -DBOARD=esp32c3_speakerbot set-target esp32c3 build"
#endif

// ── LED polarity helpers (computed from board_config.h values) ───────────────
#if LED_ACTIVE_LOW
  #define LED_ON  0
  #define LED_OFF 1
#else
  #define LED_ON  1
  #define LED_OFF 0
#endif

#endif // CONFIG_H
