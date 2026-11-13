/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Main-loop breadcrumb: how many passes ran and which task of the loop took
 * longest, so a kernel that later stops leaves a last "loop" line (every 10 s) in CEC.TXT showing whether the loop
 * was still turning and which task was slow just before. Pure; a few adds and compares per task. */
#ifndef LOOPTRACE_H
#define LOOPTRACE_H
#include <stdint.h>
#define LOOP_PHASES 8u
struct looptrace { uint32_t passes, max_us[LOOP_PHASES]; };
extern struct looptrace looptrace0;
static inline void looptrace_note(unsigned phase, uint32_t us) {
    if (phase < LOOP_PHASES && us > looptrace0.max_us[phase]) looptrace0.max_us[phase] = us;
}
/* Writes "loop N passes, max U us in task P (name), os 0|1" and clears the counters; returns the length. */
unsigned looptrace_take(char *out, unsigned cap, int os_running);
#endif
