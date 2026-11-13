/* SPDX-License-Identifier: MIT; the app's display, time and input API (slice 4b), see docs/apps.md: screen modes and the
 * surface lease (direct pixels, exclusive with the DOM-lite UI), the microsecond clock, sleep(ms) and Math.random, and raw
 * input queues (key, attach, detach, reset). jsrt.c registers it and supplies the checks below. */
#pragma once
#include "quickjs.h"
#include <stdint.h>
#define JSDEV_POLL_NS 10000000ull         /* input is looked at every 10 ms while a device is attached */
#define JSDEV_SLEEP_CHUNK_NS 100000000ull /* the longest the loop ever sleeps in the kernel: the heartbeat limit is 2 s */
#define JSDEV_EVENTS_PER_STEP 128u        /* input events handled in one turn; the rest wait for the next turn */
void jsdev_init(JSRuntime *rt, JSContext *ctx, JSValueConst native);
int jsdev_leased(void);
/* One input event from plat_event. Returns 1 when held keys were released (a reset): drain them with jsdev_released_next. */
unsigned jsdev_input(uint16_t type, uint16_t code, int32_t value);
int jsdev_released_next(void);          /* a Linux key code released by the last reset, or -1 */
void jsdev_sleeps_due(uint64_t now);    /* resolves the sleep() promises that are due */
uint64_t jsdev_wake(uint64_t now);      /* when the loop must look again: the poll interval, or an earlier sleep, never past the chunk */
/* From jsrt.c. */
int jsrt_need(JSContext *c, uint32_t any_cap, const char *what);
JSValue jsrt_call_native(const char *name, int argc, JSValueConst *argv);
int jsrt_game_active(void);
void jsrt_page_mode(uint32_t *w, uint32_t *h, uint32_t *d);
void jsrt_restore_page_mode(void);      /* the page's mode again, all of it repainted */
