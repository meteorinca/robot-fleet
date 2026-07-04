#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include "soc/soc_caps.h"

#if SOC_TOUCH_SENSOR_SUPPORTED
void touch_input_start(void);
#endif

#endif
