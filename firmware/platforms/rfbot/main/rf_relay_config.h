// rf_relay_config.h
// ============================================================================
//  Photodetector RF Relay Configuration
//
//  Configure the default RF code to trigger on and the default target API endpoint.
//  Note: The target host/URL can also be changed dynamically via the Web UI or HTTP API
//  (e.g., speakerbot5.local, speakerbot12.local/bark, 192.168.1.50/bark).
//
//  RF_RELAY_CODE   : The 24-bit decimal code sent by photodetector TX (default: 123456, displays as 0x1E240 in hex)
//  RF_RELAY_TARGET : Default target SpeakerBot host/URL (e.g., "speakerbot1.local/bark", "speakerbot5.local/bark")
//
//  No other file needs editing.
// ============================================================================

#ifndef RF_RELAY_CODE
#define RF_RELAY_CODE   123456
#endif

#ifndef RF_RELAY_TARGET
#define RF_RELAY_TARGET "speakerbot5.local/bark"
#endif
