#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <stdbool.h>

void ultrasonic_init(void);
void ultrasonic_set_active(bool active);
bool ultrasonic_is_active(void);
float ultrasonic_get_distance(void);

#endif
