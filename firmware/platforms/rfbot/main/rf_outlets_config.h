// rf_outlets_config.h
// ============================================================================
//  RF Outlet Button Definitions
//
//  Format:  RF_OUTLET("Label", on_decimal_code, off_decimal_code)
//
//  All codes are DECIMAL (same numbers shown by the working transmitter).
//  Protocol 1, 24 bits, 185 µs pulse is assumed — matching your remote.
//
//  To add an outlet: copy any line below and fill in the label + codes.
//  To remove an outlet: comment out or delete its line.
//  No other file needs editing.
// ============================================================================

RF_OUTLET("Alpha",   5576451, 5576460)
RF_OUTLET("Bravo",   5584131, 5584140)
RF_OUTLET("Foxtrot", 1381827, 1381836)
