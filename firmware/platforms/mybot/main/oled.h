#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OLED_MODE_NORMAL = 0,
    OLED_MODE_PONG_H = 1,
    OLED_MODE_PONG_V = 2,
    OLED_MODE_FLAPPY = 3,
    OLED_MODE_DINO = 4,
    OLED_MODE_SNAKE = 5,
    OLED_MODE_PACMAN = 6,
    OLED_MODE_FROGGER = 7,
    OLED_MODE_RACING = 8,
    OLED_MODE_MATH = 9,
    OLED_MODE_3D_SHOWCASE = 10,
    OLED_MODE_MENU = 11,
    OLED_MODE_TRUTH_TABLE = 12,
    OLED_MODE_BUZZER_PIANO = 13,
    OLED_MODE_US_SHOOTER = 14,
    // Fun button-triggered animations
    OLED_MODE_MARIO_DANCE = 15,
    OLED_MODE_FIREWORKS = 16,
    OLED_MODE_MATRIX_RAIN = 17,
    OLED_MODE_SPACE_INVADER = 18,
    OLED_MODE_HEARTBEAT = 19,
    OLED_MODE_ULTRASONIC_VIEW = 20
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
