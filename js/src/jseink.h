/* SPDX-License-Identifier: MIT; the app's e-ink panel (slice 4d), see docs/apps.md: one owner, a 3904-byte 1-bit page, updates
 * paced by the kernel's refresh limits (a page that is too soon is queued, the newest kept, and retried; the panel is never
 * spammed). jsrt.c registers it and calls jseink_pump from every step. */
#pragma once
#include "quickjs.h"
#include <stdint.h>
#define JSEINK_RETRY_NS 250000000ull /* after a kernel EAGAIN the queued update is tried again this much later */
void jseink_init(JSContext *ctx, JSValueConst native);
void jseink_pump(uint64_t now);
uint64_t jseink_wake(uint64_t now, uint64_t wake); /* wake, or the time the queued update may be sent */
