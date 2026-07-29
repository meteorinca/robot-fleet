#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void dog_actions_start(void);
bool dog_action_send(const char *name);

#ifdef __cplusplus
}
#endif
