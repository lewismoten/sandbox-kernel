/* SPDX-License-Identifier: MIT; original 64-bit monotonic clock (clock.h). */
#include "kernel.h"
#include "clock.h"
static uint64_t total; static uint32_t last; static bool started;
uint64_t clock_us(void) {
 uint32_t t=micros();
 if(!started) {started=true;last=t;total=t;return total;}
 total+=(uint32_t)(t-last);last=t;return total;
}
