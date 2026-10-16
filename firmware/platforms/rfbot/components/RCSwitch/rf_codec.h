#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t high, low; } HighLow;
typedef struct {
    uint16_t pulseLength;
    HighLow syncFactor, zero, one;
    bool invertedSignal;
} Protocol;

#define RF_PROTOCOL_COUNT 12
#define RF_MAX_BITS 32
extern const Protocol rf_protocols[RF_PROTOCOL_COUNT];

typedef struct { uint16_t duration; uint8_t level; } rf_pulse_t;
typedef struct {
    uint32_t code;
    unsigned bits, protocol, pulse;
    size_t consumed;
} rf_decoded_t;

// Decode only complete frames bounded by a sync pair. Linear scan, at most
// 32 data bits per candidate. No allocations, interrupts, or network work.
bool rf_decode(const rf_pulse_t *pulses, size_t count, rf_decoded_t *out);
