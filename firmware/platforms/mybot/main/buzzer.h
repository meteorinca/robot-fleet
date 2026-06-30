#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>

// Initializes the buzzer on BUZZER_PIN (if defined in board_config.h)
void buzzer_init(void);

// Plays a tone of `freq_hz` for `duration_ms` without blocking the main loop
void buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms);

// Demo Sounds
void buzzer_demo_coin(void);
void buzzer_demo_gameover(void);
void buzzer_demo_siren(void);
void buzzer_demo_laser(void);
void buzzer_demo_startup(void);
void buzzer_demo_wifi_connected(void);
void buzzer_demo_xfiles(void);
void buzzer_demo_mario(void);
void buzzer_demo_1up(void);

#endif // BUZZER_H
