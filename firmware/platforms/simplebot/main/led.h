#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

void led_init(void);
void led_set(bool on);
void led_action_set(bool state);
void led_action_toggle(void);
void led_register_manual_control(void);
void led_start_heartbeat(EventGroupHandle_t wifi_events, EventBits_t connected_bit);

#endif
