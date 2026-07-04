// main/motor.h
// ============================================================================
//  CarBot L298N Dual Motor Driver
//
//  Motor A = Steering (open-loop, no feedback)
//    - Direction: IN1 / IN2 GPIO levels
//    - Speed: PWM duty on active direction pin via LEDC
//    - Steering position tracked internally as 0-100 (50 = center)
//
//  Motor B = Drive (RWD — two rear motors wired in parallel)
//    - Direction: IN3 / IN4 GPIO levels
//    - Speed: PWM duty on active direction pin via LEDC
// ============================================================================
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Initialization ────────────────────────────────────────────────────────────
// Call once in app_main before any motor functions.
void motor_init(void);

// ── Drive Motor (Motor B — RWD) ───────────────────────────────────────────────
// speed: 0-100 (percentage)
// Call motor_drive_forward / motor_drive_backward / motor_drive_brake / motor_drive_coast
void motor_drive_forward(uint8_t speed_pct);
void motor_drive_backward(uint8_t speed_pct);
void motor_drive_brake(void);   // active brake (both IN3+IN4 HIGH)
void motor_drive_coast(void);   // free-wheel (both IN3+IN4 LOW)

// ── Steering Motor (Motor A — open loop) ─────────────────────────────────────
// target_pos: 0 (full left) to 100 (full right), 50 = center
// This is open-loop: the firmware tracks estimated position and applies
// timed pulses to move incrementally.  Call motor_steer_set_center() to
// re-calibrate the home position.
void motor_steer_set(uint8_t target_pos);

// Immediately stop the steering motor (hold current estimated position)
void motor_steer_stop(void);

// Reset the estimated steering position to center without moving
void motor_steer_reset_center(void);

// Run the steering motor in raw direction at a given speed (for calibration)
void motor_steer_raw(bool right, uint8_t speed_pct);

// Get the current estimated steering position (0-100)
uint8_t motor_steer_get_pos(void);

// ── Combined drive command (for web / ESP-NOW) ────────────────────────────────
// throttle: -100 (full reverse) to +100 (full forward)  0 = brake
// steer:     0   (full left)   to  100 (full right)    50 = straight
void motor_set(int8_t throttle, uint8_t steer);

// ── Status ────────────────────────────────────────────────────────────────────
int8_t  motor_drive_get_throttle(void);
bool    motor_is_braking(void);

#ifdef __cplusplus
}
#endif
