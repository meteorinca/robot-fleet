#ifndef WS2812_H
#define WS2812_H

// ============================================================================
//  ws2812.h — WS2812 NeoPixel strip driver (RMT-based, IDF v5)
//
//  Exposes the same API as led.c so main.c / webserver.c need no changes.
//  Only compiled when WS2812_NUM_LEDS is defined in the active board_config.h
// ============================================================================

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// ── LED-compatible API (mirrors led.h exactly) ───────────────────────────────
void led_init(void);
void led_set(bool on);
void led_register_manual_control(void);
void led_action_set(bool state);
void led_action_toggle(void);
void led_start_heartbeat(EventGroupHandle_t wifi_events, EventBits_t connected_bit);

// ── WS2812-specific extras ───────────────────────────────────────────────────
// Set all LEDs to a raw 0x00RRGGBB colour (brightness already baked in)
void ws2812_set_all(uint32_t rgb);
// Set individual pixel
void ws2812_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);
// Push changes to strip
void ws2812_show(void);
// Turn all pixels off
void ws2812_clear(void);

#endif // WS2812_H
