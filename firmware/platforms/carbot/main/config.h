// main/config.h
// ============================================================================
//  MOJ ESP32 Template — Board Router
//
//  This file selects the correct board header based on the BOARD cmake cache
//  variable, which is set at configure time:
//
//    idf.py set-target esp32c3 build
//
//  See README_BOARDS.md for the full workflow.
// ============================================================================
#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/soc_caps.h"

// ── Active board selection (injected by CMakeLists.txt via -DBOARD_CONFIG=) ─
#ifdef BOARD_CONFIG_HEADER
  #include BOARD_CONFIG_HEADER         // e.g. "../boards/esp32c3_dog/board_config.h"
#else
  #error "No BOARD selected. Run: idf.py -DBOARD=<name> set-target <chip> build"
  //
  // Available boards:
  //   esp32c3_dog       → idf.py set-target esp32c3 build
  //
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
