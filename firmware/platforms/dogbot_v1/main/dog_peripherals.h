#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef DISP_MOSI_GPIO

void dog_peripherals_init(void);
void dog_audio_play_chunk(const uint8_t *data, size_t size);
void dog_audio_play_async(uint8_t *data, size_t size); // takes ownership of malloc'd data
void dog_audio_play_tone(void);
void dog_audio_play_paulbot(void);

void dog_set_eye_mood(int mood);
void dog_set_oled_text(const char* msg, int duration_ms);

#endif
