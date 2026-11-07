/* SPDX-License-Identifier: MIT; PIO DWC2 backend never uses USB DMA. */
#pragma once
#include "kernel.h"
static inline void data_clean(const void *p,unsigned n) {(void)p;(void)n;barrier();}
static inline void data_invalidate(const void *p,unsigned n) {(void)p;(void)n;barrier();}
static inline void data_clean_and_invalidate(const void *p,unsigned n) {(void)p;(void)n;barrier();}
