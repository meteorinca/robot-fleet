// main/motor.h
// ============================================================================
//  CamBot — L298N Motor Driver
//
//  Two motor channels:
//    Steering  — L298N IN1/IN2  → MOTOR_STEER_IN1 / MOTOR_STEER_IN2
//    Drive     — L298N IN3/IN4  → MOTOR_DRIVE_IN3 / MOTOR_DRIVE_IN4
//
//  Speed/direction values: -100 (full reverse) … 0 (stop) … +100 (full fwd)
//  The L298N ENA/ENB are assumed jumpered HIGH (always-on enable), so direction
//  is controlled purely by toggling IN1-IN4. No PWM required for basic RC use.
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Initialise GPIO pins for motor control (call once at startup)
void motor_init(void);

// Set steering: -100 = full left, 0 = centre, +100 = full right
void motor_steer(int value);

// Set drive:    -100 = full reverse, 0 = stop, +100 = full forward
void motor_drive(int value);

// Immediate emergency stop — both motors off
void motor_stop_all(void);

// Query current values
int motor_get_steer(void);
int motor_get_drive(void);
