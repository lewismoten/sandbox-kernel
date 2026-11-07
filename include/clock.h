/* SPDX-License-Identifier: MIT; one monotonic clock for the kernel: the
 * 32-bit system timer (1 MHz) extended to 64 bits. The main loop calls it
 * every pass, far more often than the 71-minute wrap, so no wrap is missed. */
#pragma once
#include <stdint.h>
uint64_t clock_us(void);
