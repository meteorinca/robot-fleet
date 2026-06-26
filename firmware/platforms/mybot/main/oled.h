#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void oled_init(void);
void oled_set_text(const char* msg, int duration_ms);

#ifdef __cplusplus
}
#endif
