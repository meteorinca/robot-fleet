#ifndef RF_H
#define RF_H

#include <stdint.h>

#ifdef HAS_RF
void rf_module_init(void);
void rf_send_code(uint32_t code, unsigned int bit_length);
void rf_start_receiver(void);
#else
static inline void rf_module_init(void) {}
static inline void rf_send_code(uint32_t code, unsigned int bit_length) { (void)code; (void)bit_length; }
static inline void rf_start_receiver(void) {}
#endif

#endif // RF_H
