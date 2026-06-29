#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OLED_MODE_NORMAL = 0,
    OLED_MODE_PONG
} oled_mode_t;

typedef enum {
    EYE_EMOTION_NORMAL = 0,
    EYE_EMOTION_MAD,
    EYE_EMOTION_SAD,
    EYE_EMOTION_SLEEPY,
    EYE_EMOTION_SURPRISED,
    EYE_EMOTION_COUNT
} eye_emotion_t;

void oled_init(void);
void oled_set_text(const char* msg, int duration_ms);

void oled_set_mode(oled_mode_t mode);
oled_mode_t oled_get_mode(void);
void oled_set_emotion(eye_emotion_t emotion);
eye_emotion_t oled_get_emotion(void);
void oled_set_paddle_input(bool left, bool right);

#ifdef __cplusplus
}
#endif
