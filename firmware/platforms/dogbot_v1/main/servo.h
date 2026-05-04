#ifndef SERVO_H
#define SERVO_H

void servo_init(void);
void servo_set_angle(int servo_num, int angle);
void servo_action_set(int servo, int angle);
// Async: queues move-to-target then return-to-neutral (non-blocking)
void servo_quick_action(int servo, int target_angle, int neutral_angle);
void servo_worker_start(void);

// Runtime helpers — values come from board_config.h at compile time
int  servo_count(void);          // returns SERVO_COUNT (2 or 4)
int  servo_neutral(int servo_num); // returns POSn_NEUTRAL for servo n

#endif
