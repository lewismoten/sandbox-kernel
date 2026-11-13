/* SPDX-License-Identifier: MIT; the app's serial link (slice 4d), see docs/apps.md: one owner, partial reads and writes, and a
 * 'data' event fed by a bounded poll (a flood stays in the kernel's buffer, which counts what it drops). */
#pragma once
#include "quickjs.h"
#include <stdint.h>
#define JSSERIAL_POLL_NS 20000000ull /* the link is looked at every 20 ms while a 'data' listener exists */
#define JSSERIAL_CHUNK 256u          /* bytes per read and per 'data' event */
#define JSSERIAL_PER_STEP 1024u      /* bytes delivered to the app in one turn: a flood cannot starve the loop */
void jsserial_init(JSContext *ctx, JSValueConst native);
void jsserial_pump(uint64_t now);
uint64_t jsserial_wake(uint64_t now, uint64_t wake);
