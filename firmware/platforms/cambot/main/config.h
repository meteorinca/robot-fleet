// main/config.h
// ============================================================================
//  CamBot — Board Router
//
//  Selects the correct board header based on the BOARD cmake cache variable:
//
//    idf.py set-target esp32s3 build
//    idf.py -DBOARD=esp32s3_cambot -DDEVICE_NUMBER=2 set-target esp32s3 build
//
// ============================================================================
#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"
#include "soc/soc_caps.h"

// ── Active board selection (injected by CMakeLists.txt via -DBOARD_CONFIG=) ─
#ifdef BOARD_CONFIG_HEADER
  #include BOARD_CONFIG_HEADER         // e.g. "boards/esp32s3_cambot/board_config.h"
#else
  #error "No BOARD selected. Run: idf.py set-target esp32s3 build"
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
