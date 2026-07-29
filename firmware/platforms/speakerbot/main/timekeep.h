#ifndef TIMEKEEP_H
#define TIMEKEEP_H

#include <stdbool.h>
#include <time.h>
#include <sys/time.h>

// Initialize SNTP and timezone
void timekeep_init(void);

// Returns true once NTP has synced at least once
bool timekeep_is_synced(void);

// Get current time as Unix timestamp (seconds since epoch)
time_t timekeep_now(void);

// Format current time into buf (e.g. "2026-04-17 14:30:05")
void timekeep_format(char *buf, size_t len);

// Force the internal time (e.g. from browser)
void timekeep_set_time(time_t epoch);

// ── Scheduler ──
// Schedule a named action at a specific Unix timestamp
void timekeep_schedule(const char *action, time_t execute_at);

// Schedule with millisecond sub-second precision (extra_ms added to execute_at)
void timekeep_schedule_ms(const char *action, time_t execute_at, int32_t extra_ms);

// Start the scheduler background task
void timekeep_start_scheduler(void);

#endif
