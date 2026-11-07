#pragma once
#include <stdint.h>
uint32_t tud_cdc_available(void);
uint32_t tud_cdc_read(void *,uint32_t);
void tud_cdc_read_flush(void);
