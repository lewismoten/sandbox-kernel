/* SPDX-License-Identifier: MIT; singleton cooperative cache/save publication. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define CACHE_COMMIT_SLICE_US 1000u
/* Start takes ownership of kernel-owned stable bytes until pending becomes false.
   No media access here. Only the sys writer may call this. */
int cache_commit_start(const char *game,const char *name,const uint8_t *data,uint32_t bytes);
void cache_commit_task(void); /* foreground main loop only, never IRQ/service_io */
void cache_commit_checkpoint(bool boundary); /* worker only; boundary always yields */
bool cache_commit_pending(void);
bool cache_commit_blocked(void); /* pending and outside the worker */
uint32_t cache_commit_state(void); /* 0 idle, 1 pending, 2 done, 3 error */
int cache_commit_error(void); /* negative errno, retained until next submission */
