#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <stdbool.h>

#if defined(ENABLE_ULTRASONIC) && ENABLE_ULTRASONIC

void ultrasonic_init(void);
void ultrasonic_set_active(bool active);
bool ultrasonic_is_active(void);
float ultrasonic_get_distance(void);

#else

static inline void ultrasonic_init(void) {}
static inline void ultrasonic_set_active(bool active) { (void)active; }
static inline bool ultrasonic_is_active(void) { return false; }
static inline float ultrasonic_get_distance(void) { return 0.0f; }

#endif

#endif // ULTRASONIC_H
