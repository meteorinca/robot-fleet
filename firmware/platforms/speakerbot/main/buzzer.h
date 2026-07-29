#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
#include "dog_peripherals.h"

static inline void buzzer_init(void) {}

static inline void buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    speaker_play_tone(freq_hz, duration_ms);
}

static inline void buzzer_demo_coin(void) { speaker_play_tone(988, 80); speaker_play_tone(1319, 250); }
static inline void buzzer_demo_gameover(void) { speaker_play_tone(300, 150); speaker_play_tone(200, 250); }
static inline void buzzer_demo_siren(void) { speaker_play_tone(800, 100); speaker_play_tone(600, 100); }
static inline void buzzer_demo_laser(void) { speaker_play_tone(1500, 40); }
static inline void buzzer_demo_startup(void) { speaker_play_tone(523, 100); speaker_play_tone(659, 100); speaker_play_tone(784, 150); }
static inline void buzzer_demo_wifi_connected(void) { speaker_play_tone(1046, 100); }
static inline void buzzer_demo_xfiles(void) { speaker_play_tone(440, 200); }
static inline void buzzer_demo_mario(void) { speaker_play_tone(660, 100); speaker_play_tone(660, 100); }
static inline void buzzer_demo_1up(void) { speaker_play_tone(330, 80); speaker_play_tone(660, 120); }

#endif // BUZZER_H
