/* SPDX-License-Identifier: MIT */
#pragma once
#include "kernel.h"
static inline void BP_EnableIRQ(unsigned n) { REG(PERIPH+0xb210+(n/32)*4)=1u<<(n%32);barrier(); }
static inline void BP_DisableIRQ(unsigned n) { REG(PERIPH+0xb21c+(n/32)*4)=1u<<(n%32);barrier(); }
