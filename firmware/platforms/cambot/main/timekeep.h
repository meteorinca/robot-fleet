#ifndef TIMEKEEP_H
#define TIMEKEEP_H

#include <stdbool.h>
#include <time.h>
#include <sys/time.h>

void   timekeep_init(void);
bool   timekeep_is_synced(void);
time_t timekeep_now(void);
void   timekeep_format(char *buf, size_t len);
void   timekeep_set_time(time_t epoch);
void   timekeep_schedule(const char *action, time_t execute_at);
void   timekeep_schedule_ms(const char *action, time_t execute_at, int32_t extra_ms);
void   timekeep_start_scheduler(void);

#endif
