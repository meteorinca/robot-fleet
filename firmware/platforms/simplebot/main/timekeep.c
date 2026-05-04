#include "timekeep.h"
#include "webserver.h"
#include "config.h"
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static bool s_synced = false;

// ── Scheduled actions ──
typedef struct {
    time_t execute_at;       // second-resolution trigger
    int32_t execute_at_ms;   // additional milliseconds offset (0-999)
    char   action[32];       // e.g. "hi", "wiggle", "s1_90", "tts:hello"
    bool   active;
} sched_entry_t;

static sched_entry_t s_schedule[MAX_SCHEDULED_ACTIONS];
static SemaphoreHandle_t s_sched_mutex;

// Callback when SNTP syncs
static void time_sync_cb(struct timeval *tv) {
    s_synced = true;
    char buf[32];
    timekeep_format(buf, sizeof(buf));
    ESP_LOGI("TIME", "NTP synced: %s", buf);
}

void timekeep_init(void) {
    // Set timezone before anything else
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // SNTP init — polls once per hour by default
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_set_sync_interval(3600000);   // 1 hour between re-syncs (ms)
    esp_sntp_init();

    s_sched_mutex = xSemaphoreCreateMutex();
    memset(s_schedule, 0, sizeof(s_schedule));

    ESP_LOGI("TIME", "SNTP started — server: %s", NTP_SERVER);
}

bool timekeep_is_synced(void) {
    return s_synced;
}

time_t timekeep_now(void) {
    time_t now;
    time(&now);
    return now;
}

void timekeep_format(char *buf, size_t len) {
    time_t now = timekeep_now();
    struct tm ti;
    localtime_r(&now, &ti);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &ti);
}

void timekeep_set_time(time_t epoch) {
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_synced = true;
}

void timekeep_schedule(const char *action, time_t execute_at) {
    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SCHEDULED_ACTIONS; i++) {
        if (!s_schedule[i].active) {
            s_schedule[i].execute_at    = execute_at;
            s_schedule[i].execute_at_ms = 0;
            strncpy(s_schedule[i].action, action, sizeof(s_schedule[i].action) - 1);
            s_schedule[i].action[sizeof(s_schedule[i].action) - 1] = '\0';
            s_schedule[i].active = true;
            ESP_LOGI("SCHED", "Scheduled '%s' at %ld", action, (long)execute_at);
            break;
        }
    }
    xSemaphoreGive(s_sched_mutex);
}

// Schedule with millisecond precision: execute_at_ms adds 0-999ms after execute_at
void timekeep_schedule_ms(const char *action, time_t execute_at, int32_t extra_ms) {
    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SCHEDULED_ACTIONS; i++) {
        if (!s_schedule[i].active) {
            s_schedule[i].execute_at    = execute_at + extra_ms / 1000;
            s_schedule[i].execute_at_ms = extra_ms % 1000;
            strncpy(s_schedule[i].action, action, sizeof(s_schedule[i].action) - 1);
            s_schedule[i].action[sizeof(s_schedule[i].action) - 1] = '\0';
            s_schedule[i].active = true;
            ESP_LOGI("SCHED", "Scheduled '%s' at %ld+%ldms",
                     action, (long)execute_at, (long)extra_ms);
            break;
        }
    }
    xSemaphoreGive(s_sched_mutex);
}

static void scheduler_task(void *pvParameters) {
    // 100ms tick — gives ~100ms precision for fleet-synchronized moves.
    // Actual servo dispatch is still non-blocking (queued to dog_task).
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        // if (!s_synced) continue; // removed to allow relative scheduling without internet

        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t  now_s  = tv.tv_sec;
        int32_t now_ms = tv.tv_usec / 1000;

        xSemaphoreTake(s_sched_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_SCHEDULED_ACTIONS; i++) {
            if (!s_schedule[i].active) continue;
            // Fire when wall-clock has passed the target second+ms
            bool due = (now_s > s_schedule[i].execute_at) ||
                       (now_s == s_schedule[i].execute_at &&
                        now_ms >= s_schedule[i].execute_at_ms);
            if (due) {
                ESP_LOGI("SCHED", "Firing '%s'", s_schedule[i].action);
                s_schedule[i].active = false;
                xSemaphoreGive(s_sched_mutex);
                execute_named_action(s_schedule[i].action);
                xSemaphoreTake(s_sched_mutex, portMAX_DELAY);
            }
        }
        xSemaphoreGive(s_sched_mutex);
    }
}

void timekeep_start_scheduler(void) {
    xTaskCreate(scheduler_task, "sched", 3072, NULL, 3, NULL);
}
