// main/dog_actions.c
// All 12 preset dog animations adapted from the ESP-Hi example's
// servo_dog_ctrl.c.  Uses our own servo_set_angle() instead of iot_servo.
//
// Servo mapping (from board_config.h):
//   FL = servo 1 (GPIO 21)   FR = servo 2 (GPIO 19)
//   BL = servo 3 (GPIO 20)   BR = servo 4 (GPIO 18)
//
// Neutral angles (from board_config.h): all at 90°.
//
// The animations use the same math as the ESP-Hi example but drive
// angles relative to our 90° neutral rather than ESP-Hi's 70/110 neutrals.
// FL and FR are mirrored from the example (FL goes down as angle decreases,
// FR goes down as angle increases on the ESP-Hi geometry).

#include "dog_actions.h"
#include "servo.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdbool.h>

#define TAG "DOG"

// ── Servo shorthand wrappers ──────────────────────────────────────────────────
// servo_set_angle() is declared in servo.h — 1-indexed, 0-180 degrees.

static inline void fl(int a) { servo_set_angle(1, a < 0 ? 0 : a > 180 ? 180 : a); }
static inline void fr(int a) { servo_set_angle(2, a < 0 ? 0 : a > 180 ? 180 : a); }
static inline void bl(int a) { servo_set_angle(3, a < 0 ? 0 : a > 180 ? 180 : a); }
static inline void br(int a) { servo_set_angle(4, a < 0 ? 0 : a > 180 ? 180 : a); }
static inline void all_neutral(void) { fl(90); fr(90); bl(90); br(90); }
static inline void dly(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// Non-blocking delay — peek queue; if a new command arrived, abort current anim.
static QueueHandle_t s_queue;
#define PEEK_DELAY(ms)  \
    do { \
        char _buf[32]; \
        if (xQueuePeek(s_queue, _buf, pdMS_TO_TICKS(ms)) == pdTRUE) return; \
    } while (0)

// ── Neutral angles (from board_config.h defaults) ────────────────────────────
#define N 90   // all servos neutral = 90°
// ESP-Hi geometry: FL↓=decreasing, FR↓=increasing, BL↓=increasing, BR↓=decreasing
// Forward step offsets (same ±20 as example):
#define FL_FWD  (N - 20)
#define FL_BWD  (N + 20)
#define FR_FWD  (N + 20)
#define FR_BWD  (N - 20)
#define BL_FWD  (N + 20)
#define BL_BWD  (N - 20)
#define BR_FWD  (N - 20)
#define BR_BWD  (N + 20)
#define BOW     50          // bow offset (front legs forward, back legs back)
#define STEP_OFF 5

// ── Individual animations ─────────────────────────────────────────────────────

static void anim_forward(void) {
    int speed = 33;
    int step_delay = 500 / speed;  // ~15ms per step — smooth for servos
    for (int step = 0; step < 2; step++) {
        for (int i = 0; i < 40; i++) {
            fl(FL_BWD - i); br(BR_FWD + i);
            bl(BL_BWD + i - STEP_OFF); fr(FR_FWD - i - STEP_OFF);
            PEEK_DELAY(step_delay);
        }
        PEEK_DELAY(50);
        for (int i = 0; i < 40; i++) {
            fl(FL_FWD + i); br(BR_BWD - i);
            bl(BL_FWD - i + STEP_OFF); fr(FR_BWD + i + STEP_OFF);
            PEEK_DELAY(step_delay);
        }
        PEEK_DELAY(50);
    }
    for (int i = 0; i < 20; i++) {
        fl(FL_BWD - i); br(BR_FWD + i);
        bl(BL_BWD + i); fr(FR_FWD - i);
        PEEK_DELAY(step_delay);
    }
    all_neutral();
}

static void anim_backward(void) {
    int speed = 33;
    int step_delay = 500 / speed;
    for (int step = 0; step < 2; step++) {
        for (int i = 0; i < 40; i++) {
            bl(BL_FWD - i); fr(FR_BWD + i);
            fl(FL_FWD + i - STEP_OFF); br(BR_BWD - i - STEP_OFF);
            PEEK_DELAY(step_delay);
        }
        PEEK_DELAY(50);
        for (int i = 0; i < 40; i++) {
            bl(BL_BWD + i); fr(FR_FWD - i);
            fl(FL_BWD - i + STEP_OFF); br(BR_FWD + i + STEP_OFF);
            PEEK_DELAY(step_delay);
        }
        PEEK_DELAY(50);
    }
    for (int i = 0; i < 20; i++) {
        bl(BL_FWD - i); fr(FR_BWD + i);
        fl(FL_FWD + i); br(BR_BWD - i);
        PEEK_DELAY(step_delay);
    }
    all_neutral();
}

static void anim_turn_left(void) {
    int step_delay = 500 / 33;
    for (int step = 0; step < 2; step++) {
        for (int i = 0; i < 40; i++) {
            fl(FL_BWD - i + STEP_OFF); br(BR_BWD - i + STEP_OFF);
            bl(BL_BWD + i); fr(FR_BWD + i);
            PEEK_DELAY(step_delay);
        }
        for (int i = 0; i < 40; i++) {
            fl(FL_FWD + i - STEP_OFF); br(BR_FWD + i - STEP_OFF);
            bl(BL_FWD - i); fr(FR_FWD - i);
            PEEK_DELAY(step_delay);
        }
    }
    all_neutral();
}

static void anim_turn_right(void) {
    int step_delay = 500 / 33;
    for (int step = 0; step < 2; step++) {
        for (int i = 0; i < 40; i++) {
            fl(FL_FWD + i); br(BR_FWD + i);
            bl(BL_FWD - i + STEP_OFF); fr(FR_FWD - i + STEP_OFF);
            PEEK_DELAY(step_delay);
        }
        for (int i = 0; i < 40; i++) {
            fl(FL_BWD - i - STEP_OFF); br(BR_BWD - i - STEP_OFF);
            bl(BL_BWD + i); fr(FR_BWD + i);
            PEEK_DELAY(step_delay);
        }
    }
    all_neutral();
}

static void anim_lay_down(void) {
    for (int i = 0; i < 60; i++) {
        fl(N - i); fr(N + i);
        bl(N + i); br(N - i);
        dly(10);
    }
}

static void anim_bow(void) {
    int step_delay = 500 / 33;
    for (int i = 0; i < BOW; i++) {
        fl(N - i); fr(N + i);
        bl(N - i); br(N + i);
        PEEK_DELAY(step_delay);
    }
    PEEK_DELAY(500);
    for (int i = 0; i < BOW; i++) {
        fl(N - BOW + i); fr(N + BOW - i);
        bl(N - BOW + i); br(N + BOW - i);
        PEEK_DELAY(step_delay);
    }
    all_neutral();
}

static void anim_lean_back(void) {
    int step_delay = 500 / 33;
    for (int i = 0; i < BOW; i++) {
        fl(N + i); fr(N - i);
        bl(N + i); br(N - i);
        PEEK_DELAY(step_delay);
    }
    PEEK_DELAY(500);
    for (int i = 0; i < BOW; i++) {
        fl(N + BOW - i); fr(N - BOW + i);
        bl(N + BOW - i); br(N - BOW + i);
        PEEK_DELAY(step_delay);
    }
    all_neutral();
}

static void anim_wiggle(void) {
    // bow_and_lean_back — slowed to 15ms/step so servos can follow
    int step_delay = 15;
    for (int i = 0; i < BOW; i++) {
        fl(N - i); fr(N + i); bl(N - i); br(N + i);
        PEEK_DELAY(step_delay);
    }
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < BOW * 2; i++) {
            fl(N - BOW + i); fr(N + BOW - i);
            bl(N - BOW + i); br(N + BOW - i);
            PEEK_DELAY(step_delay);
        }
        for (int i = 0; i < BOW * 2; i++) {
            fl(N + BOW - i); fr(N - BOW + i);
            bl(N + BOW - i); br(N - BOW + i);
            PEEK_DELAY(step_delay);
        }
    }
    for (int i = 0; i < BOW; i++) {
        fl(N - BOW + i); fr(N + BOW - i);
        bl(N - BOW + i); br(N + BOW - i);
        PEEK_DELAY(step_delay);
    }
    all_neutral();
}

static void anim_rock(void) {
    // sway_back_and_forth — 15ms steps, uses PEEK_DELAY so it can be interrupted
    int sway = 18;
    for (int i = 0; i < sway; i++) {
        fl(N - i); fr(N + i); bl(N - i); br(N + i);
        PEEK_DELAY(15);
    }
    for (int rep = 0; rep < 6; rep++) {
        for (int i = 0; i < sway * 2; i++) {
            fl(N - sway + i); fr(N + sway - i);
            bl(N - sway + i); br(N + sway - i);
            PEEK_DELAY(15);
        }
        for (int i = 0; i < sway * 2; i++) {
            fl(N + sway - i); fr(N - sway + i);
            bl(N + sway - i); br(N - sway + i);
            PEEK_DELAY(15);
        }
    }
    all_neutral();
}

static void anim_sway(void) {
    // sway_left_right
    int step_delay = 500 / 33;
    int offset = 20;
    // Start with elevated body
    fl(FL_BWD); fr(FR_BWD); bl(BL_BWD); br(BR_BWD);
    dly(20);
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < offset; i++) {
            fl(FL_BWD - i); fr(FR_BWD - i); bl(BL_BWD - i); br(BR_BWD - i);
            PEEK_DELAY(step_delay);
        }
        for (int i = 0; i < offset * 2; i++) {
            fl(FL_BWD - offset + i); fr(FR_BWD - offset + i);
            bl(BL_BWD - offset + i); br(BR_BWD - offset + i);
            PEEK_DELAY(step_delay);
        }
        for (int i = 0; i < offset; i++) {
            fl(FL_BWD + offset - i); fr(FR_BWD + offset - i);
            bl(BL_BWD + offset - i); br(BR_BWD + offset - i);
            PEEK_DELAY(step_delay);
        }
    }
    all_neutral();
}

static void anim_shake(void) {
    // shake_hand
    for (int i = 0; i < 60; i++) {
        bl(N - i); br(N + i);
        dly(8);
    }
    int start = N + 72, end_a = N + 57;
    fr(start);
    for (int j = 0; j < 10; j++) {
        for (int a = start; a >= end_a; a--) { fr(a); dly(15); }
        for (int a = end_a; a <= start; a++) { fr(a); dly(15); }
    }
    dly(3000);
    for (int a = start; a >= N; a--) { fr(a); dly(5); }
    for (int i = 0; i < 60; i++) { bl(N - 60 + i); br(N + 60 - i); dly(8); }
    all_neutral();
}

static void anim_poke(void) {
    fl(0);
    dly(20);
    for (int i = 0; i < 5; i++) {
        fr(N + i); bl(N - 10 * i); br(N + 10 * i);
        dly(10);
    }
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 20; i++) {
            fr(N + 5 + i); bl(N - 50 - i); br(N + 50 + i);
            dly(20);
        }
        for (int i = 0; i < 20; i++) {
            fr(N + 25 - i); bl(N - 70 + i); br(N + 70 - i);
            dly(20);
        }
    }
    all_neutral();
}

static void anim_kick(void) {
    // shake_back_legs
    for (int i = 0; i < 18; i++) {
        fl(N + 2*i); fr(N - 2*i); bl(N + 3*i); br(N - 3*i);
        dly(15);
    }
    for (int j = 0; j < 12; j++) {
        for (int i = 0; i < 6; i++)  { bl(N+54+i); br(N-54+i); dly(7); }
        for (int i = 0; i < 12; i++) { bl(N+54-i); br(N-54-i); dly(7); }
        for (int i = 0; i < 6; i++)  { bl(N+54+i); br(N-54+i); dly(7); }
    }
    for (int i = 0; i < 18; i++) {
        fl(N+36-2*i); fr(N-36+2*i); bl(N+54-3*i); br(N-54+3*i);
        dly(15);
    }
    all_neutral();
}

static void anim_jump_fwd(void) {
    all_neutral(); dly(300);
    fl(FL_FWD - 10); fr(FR_FWD + 10);
    bl(BL_BWD - 40); br(BR_BWD + 40);
    dly(300);
    fl(FL_BWD + 50); fr(FR_BWD - 50);
    dly(40);
    fl(FL_FWD - 50); fr(FR_FWD + 50);
    dly(20);
    bl(BL_FWD); br(BR_FWD);
    dly(150);
    fl(N); fr(N);
    dly(200);
    all_neutral();
}

static void anim_jump_bwd(void) {
    fl(FL_BWD + 20); fr(FR_BWD - 20);
    bl(BL_FWD); br(BR_FWD);
    dly(100);
    fl(FL_BWD); fr(FR_BWD); bl(BL_BWD); br(BR_BWD);
    dly(100);
    fl(N); fr(N); bl(BL_FWD); br(BR_FWD);
    dly(150);
    all_neutral();
}

// ── Action dispatch table ─────────────────────────────────────────────────────

typedef struct { const char *name; void (*fn)(void); } action_entry_t;

static const action_entry_t s_actions[] = {
    // Preset actions (numbered 1-12 by the example web UI)
    // "12" is now Stand (all_neutral) — replaces Retract
    { "1",          anim_lay_down    },  // Lie
    { "2",          anim_bow         },  // Bow
    { "3",          anim_lean_back   },  // Lean
    { "4",          anim_wiggle      },  // Wiggle
    { "5",          anim_rock        },  // Rock
    { "6",          anim_sway        },  // Sway
    { "7",          anim_shake       },  // Shake
    { "8",          anim_poke        },  // Poke
    { "9",          anim_kick        },  // Kick
    { "10",         anim_jump_fwd    },  // Jump→
    { "11",         anim_jump_bwd    },  // ←Jump
    { "12",         all_neutral      },  // Stand (was Retract)
    // Directional — F/B swapped to match physical servo geometry
    { "F",          anim_backward    },
    { "B",          anim_forward     },
    { "L",          anim_turn_left   },
    { "R",          anim_turn_right  },
    // Named aliases for scheduler / quick-buttons
    { "forward",    anim_backward    },
    { "backward",   anim_forward     },
    { "turn_left",  anim_turn_left   },
    { "turn_right", anim_turn_right  },
    { "lay",        anim_lay_down    },
    { "bow",        anim_bow         },
    { "lean",       anim_lean_back   },
    { "wiggle",     anim_wiggle      },
    { "rock",       anim_rock        },
    { "sway",       anim_sway        },
    { "shake",      anim_shake       },
    { "poke",       anim_poke        },
    { "kick",       anim_kick        },
    { "jump_fwd",   anim_jump_fwd    },
    { "jump_bwd",   anim_jump_bwd    },
    { "stand",      all_neutral      },
};
#define ACTION_COUNT (sizeof(s_actions) / sizeof(s_actions[0]))

// ── Queue + task ──────────────────────────────────────────────────────────────

static void dog_task(void *arg) {
    char name[32];
    while (1) {
        if (xQueueReceive(s_queue, name, portMAX_DELAY) == pdTRUE) {
            bool found = false;
            for (int i = 0; i < (int)ACTION_COUNT; i++) {
                if (strcmp(name, s_actions[i].name) == 0) {
                    ESP_LOGI(TAG, "Running: %s", name);
                    s_actions[i].fn();
                    found = true;
                    break;
                }
            }
            if (!found) ESP_LOGW(TAG, "Unknown action: %s", name);
        }
    }
}

void dog_actions_start(void) {
    s_queue = xQueueCreate(4, 32);
    xTaskCreate(dog_task, "dog_anim", 4096, NULL, 4, NULL);
}

bool dog_action_send(const char *name) {
    if (!name || !s_queue) return false;
    // Verify name is known
    for (int i = 0; i < (int)ACTION_COUNT; i++) {
        if (strcmp(name, s_actions[i].name) == 0) {
            char buf[32];
            strncpy(buf, name, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            xQueueSend(s_queue, buf, pdMS_TO_TICKS(50));
            return true;
        }
    }
    return false;
}
