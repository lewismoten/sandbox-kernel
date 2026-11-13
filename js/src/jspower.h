/* SPDX-License-Identifier: MIT; the app's power holds and the screen-blank state (slice 4d), see docs/apps.md. A hold is a token in
 * the kernel's 16-entry table, released at stop. The blank state (ABI 1.12) is polled at most every 100 ms, and only when something
 * needs it: the audio pump (it stops synthesizing while the screen is blank unless the app holds no-blank) or a 'blank'/'wake' listener. */
#pragma once
#include "quickjs.h"
#include <stdint.h>
#define JSPOWER_POLL_NS 100000000ull
void jspower_init(JSContext *ctx, JSValueConst native);
int jspower_dark(uint64_t now);       /* the kernel has the screen blank and the app does not hold no-blank (polls if the last look is stale) */
void jspower_pump(uint64_t now);      /* emits 'blank' / 'wake' when the state changed; only while a listener exists */
uint64_t jspower_wake(uint64_t now, uint64_t wake);
unsigned jspower_release_all(void);   /* at stop: gives back every token the app still holds; returns how many */
