#ifndef RF_H
#define RF_H

#include <stdint.h>

void rf_module_init(void);
void rf_send_code(uint32_t code, unsigned int bit_length);
void rf_start_receiver(void);

#endif
