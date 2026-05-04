// main/dog_actions.h
// Dog animation state machine — drives all 4 servos through named actions.
// Animations are lifted directly from the ESP-Hi example's servo_dog_ctrl.c
// and ported to use our project's servo_set_angle() / servo_action_set() API.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Kick off the background task that processes animation commands.
// Call once after servo_worker_start() in main().
void dog_actions_start(void);

// Queue an animation by name.  Returns false if the name is unknown.
// Safe to call from any task / HTTP handler context.
bool dog_action_send(const char *name);

#ifdef __cplusplus
}
#endif
