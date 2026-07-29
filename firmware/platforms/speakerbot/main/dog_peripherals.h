#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// SpeakerBot I2S PDM Audio Driver API
void dog_peripherals_init(void);

// Audio streaming & playback
void dog_audio_play_chunk(const uint8_t *data, size_t size);
void dog_audio_play_async(uint8_t *data, size_t size); // takes ownership of malloc'd data
void dog_audio_play_8bit(const uint8_t *data, size_t len);

// Built-in sound clips & feedback
void dog_audio_play_paulbot(void);
void dog_audio_play_bark(void);
void dog_audio_play_random(void);
void dog_audio_play_named(const char *name);
void dog_audio_play_named_ex(const char *name, int repeat, bool interrupt);

// Emergency control & audio buffer management
void dog_audio_stop(void);
bool dog_audio_is_stopped(void);
void dog_audio_reset_stop_flag(void);

// Synthesize pleasant speaker tone/beep (frequency in Hz, duration in ms)
void speaker_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void speaker_play_drum_beat(void);

#ifdef __cplusplus
}
#endif
