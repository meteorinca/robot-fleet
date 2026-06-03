#ifndef RF_H
#define RF_H

#include <stdint.h>
#include <stdbool.h>

// ── Core init / TX ───────────────────────────────────────────────────────────
void rf_module_init(void);
void rf_send_code(uint32_t code, unsigned int bit_length);
void rf_send_full(uint32_t code, unsigned int bit_length, int protocol, int pulse_length);
void rf_start_receiver(void);

// ── Learn mode ───────────────────────────────────────────────────────────────
// Captures incoming codes into a small buffer; the WebUI can retrieve them via
// rf_learn_get_codes() which clears the buffer after reading.
void rf_learn_start(void);
void rf_learn_stop(void);
bool rf_learn_active(void);

// Returns a cJSON array (caller must cJSON_Delete it) of captured codes, then
// clears the buffer.
// Each element: {"code":"1A2B3C","bits":24,"proto":1,"pulse":350}
struct cJSON;
struct cJSON *rf_learn_get_codes(void);

// ── Listen mode ───────────────────────────────────────────────────────────────
// Continuously buffers ALL received 433 MHz packets into a ring buffer so that
// the WebUI can poll /rf/poll and display them in real time.
// Normal action dispatch is suspended while listen mode is active.
void rf_listen_start(void);
void rf_listen_stop(void);
bool rf_listen_active(void);

// Returns a cJSON array of all buffered packets since the last call, then
// clears the buffer.  Caller must cJSON_Delete().
// Each element: {"code":"1A2B","bits":24,"proto":1,"pulse":350}
struct cJSON *rf_listen_get_packets(void);

#endif
