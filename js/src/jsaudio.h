/* SPDX-License-Identifier: MIT; the app's audio output (slice 4c), see docs/apps.md: one writer on the kernel's 48 kHz ring,
 * partial writes, a bounded refill pump that calls the app's onneed(frames), a square-wave tone queue, counters. jsrt.c
 * registers it, calls jsaudio_pump from every step and asks jsaudio_wake when the loop must look again. */
#pragma once
#include "quickjs.h"
#include <stdint.h>
#define JSAUDIO_POLL_NS 20000000ull   /* the ring is looked at every 20 ms while the pump or a tone is active */
#define JSAUDIO_PER_STEP 4096u        /* kernel frames synthesized or asked of the app in one turn */
#define JSAUDIO_TARGET_MS 200u        /* the ring is kept this full (its capacity is 341 ms; the loop never sleeps past 100 ms) */
void jsaudio_init(JSContext *ctx, JSValueConst native);
void jsaudio_pump(uint64_t now);
uint64_t jsaudio_wake(uint64_t now, uint64_t wake); /* wake, or an earlier time when the ring needs a look */
