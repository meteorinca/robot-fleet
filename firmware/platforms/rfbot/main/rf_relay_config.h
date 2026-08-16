// rf_relay_config.h
// ============================================================================
//  Photodetector / 433 MHz RF Relay Configuration
//
//  Format:  RF_RELAY(decimal_code, "target_host/path")
//
//  When RFBot receives the specified 24-bit decimal RF code, it automatically
//  sends an HTTP GET request to the target endpoint.
//
//  To add an RX relay rule: copy any line below and set code + target.
//  To remove a rule: comment out or delete its line.
//  You can add as many RF_RELAY(...) rules as you want below!
//  No other file needs editing.
// ============================================================================

#ifndef RF_RELAY_ENABLED_DEFAULT
#define RF_RELAY_ENABLED_DEFAULT true
#endif

// ── RX Relay Rules (Add as many RF_RELAY lines as you want!) ─────────────────
#ifndef RF_RELAY
#define RF_RELAY(code, target)
#endif

RF_RELAY(123456, "speakerbot1.local/choola")
RF_RELAY(122222, "speakerbot1.local/yes")