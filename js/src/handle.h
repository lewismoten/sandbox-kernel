/* SPDX-License-Identifier: MIT; the core's handle table: every file, audio, input, serial, e-ink, timer
 * or profile session an app holds is one entry here, so jsrt_stop can release them all and report the
 * ones the app left open. An id is (generation << 4 | slot): the generation counts up over the whole
 * process, never resets, so a stale id (closed, or from an earlier app) never reaches a newer handle.
 * One interpreter thread; no locking. */
#pragma once
#include <stddef.h>
#include <stdint.h>
enum {JSRT_H_FILE = 1, JSRT_H_AUDIO, JSRT_H_INPUT, JSRT_H_SERIAL, JSRT_H_EINK, JSRT_H_TIMER, JSRT_H_GAME, JSRT_H_DISPLAY, JSRT_H_KINDS};
#define JSRT_H_MAX 12u /* the kernel allows 16 open handles; the core and the card keep 4 */
/* Opens an entry. release(user) runs exactly once, when the entry is closed or released at stop (may be NULL).
   Returns the id, or 0 when the table is full or the kind is bad. */
uint32_t jsrt_handle_open(int kind, void (*release)(void *user), void *user);
/* The user pointer of an open entry of that kind; NULL for a stale, closed or wrong-kind id. */
void *jsrt_handle_get(uint32_t id, int kind);
/* Releases the entry: 0, or -1 for a stale id (nothing happens). */
int jsrt_handle_close(uint32_t id);
unsigned jsrt_handles_open(void);
/* Closes every open entry (stop). Returns how many there were and, in summary, "file 2, game 1" (cap bytes). */
unsigned jsrt_handles_release_all(char *summary, size_t cap);
