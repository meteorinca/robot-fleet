#include "rf_codec.h"

const Protocol rf_protocols[RF_PROTOCOL_COUNT] = {
    {350, {1,31}, {1,3}, {3,1}, false},
    {650, {1,10}, {1,2}, {2,1}, false},
    {100, {30,71}, {4,11}, {9,6}, false},
    {380, {1,6}, {1,3}, {3,1}, false},
    {500, {6,14}, {1,2}, {2,1}, false},
    {450, {23,1}, {1,2}, {2,1}, true},
    {150, {2,62}, {1,6}, {6,1}, false},
    {200, {3,130}, {7,16}, {3,16}, false},
    {200, {130,7}, {16,7}, {16,3}, true},
    {365, {18,1}, {3,1}, {1,3}, true},
    {270, {36,1}, {1,2}, {2,1}, true},
    {320, {36,1}, {1,2}, {2,1}, true},
};

static bool near(unsigned actual, unsigned expected, unsigned base) {
    unsigned delta = actual > expected ? actual - expected : expected - actual;
    return actual >= 35 && delta <= base * 60 / 100;
}

static bool pair(const rf_pulse_t *p, HighLow factors, unsigned base, unsigned level) {
    return p[0].level == level && p[1].level != level &&
           near(p[0].duration, base * factors.high, base) &&
           near(p[1].duration, base * factors.low, base);
}

static unsigned pair_error(const rf_pulse_t *p, HighLow factors, unsigned base) {
    unsigned a = base * factors.high, b = base * factors.low;
    return (p[0].duration > a ? p[0].duration-a : a-p[0].duration) +
           (p[1].duration > b ? p[1].duration-b : b-p[1].duration);
}

bool rf_decode(const rf_pulse_t *pulses, size_t count, rf_decoded_t *out) {
    // Search for trailing syncs so an initial partial frame is never accepted.
    for (size_t sync = 16; sync + 1 < count; ++sync) {
        if (pulses[sync].duration < 1500 && pulses[sync+1].duration < 1500) continue;
        bool found = false;
        unsigned best_error = 0, best_base = 1;
        rf_decoded_t best;
        for (unsigned protocol = 0; protocol < RF_PROTOCOL_COUNT; ++protocol) {
            const Protocol *p = &rf_protocols[protocol];
            unsigned longest = p->syncFactor.high > p->syncFactor.low ? 0 : 1;
            unsigned factor = longest ? p->syncFactor.low : p->syncFactor.high;
            unsigned base = pulses[sync+longest].duration / factor;
            unsigned level = p->invertedSignal ? 0 : 1;
            if (base < 50 || base > 2000 || !pair(&pulses[sync], p->syncFactor, base, level)) continue;

            uint32_t code = 0;
            unsigned bits = 0;
            unsigned error = pair_error(&pulses[sync], p->syncFactor, base);
            size_t start = sync;
            while (start >= 2 && bits < RF_MAX_BITS) {
                const rf_pulse_t *data = &pulses[start-2];
                bool zero = pair(data, p->zero, base, level);
                bool one = pair(data, p->one, base, level);
                if (zero == one) break;
                error += pair_error(data, one ? p->one : p->zero, base);
                if (one) code |= UINT32_C(1) << bits;
                ++bits;
                start -= 2;
            }
            // Require a preceding sync.
            // Reject suffixes of a corrupt packet and frames wider than 32 bits.
            bool bounded = start >= 2 &&
                pair(&pulses[start-2], p->syncFactor, base, level);
            if (bits >= 8 && bounded && (!found || error * best_base < best_error * base)) {
                found = true;
                best_error = error;
                best_base = base;
                best = (rf_decoded_t){code, bits, protocol+1, base, sync+2};
            }
        }
        if (found) { *out = best; return true; }
    }
    return false;
}
