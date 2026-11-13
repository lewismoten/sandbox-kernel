/* SPDX-License-Identifier: MIT; the JS runtime: QuickJS-ng with the
 * browser-like API of prelude.js over the element tree (dom.h), the game
 * element (the Game-Maker play core through session.h), files, timers and
 * frames on the platform's clock (plat.h). One app at a time. */
#pragma once
#include <stdint.h>
struct jsrt_stats {
  uint64_t start_ns;                  /* runtime and context made, prelude and app run */
  uint64_t js_ns, render_ns, present_ns; /* time in JS, in layout and paint, presenting (totals) */
  uint32_t frames, presents;
  uint32_t game_ticks, game_late_max_us, game_step_max_us;
  uint64_t game_play_ns;              /* time in the play phase (ticks are due only then) */
};
extern struct jsrt_stats jsrt_stats;
/* Starts the app in folder dir ("/sd/OS/APPS/HELLO": runs its MAIN.JS);
   0, or -1 (the reason is logged). heap_limit: bytes for the JS heap. */
int jsrt_start(const char *dir, uint32_t heap_limit);
/* One turn of the loop: input, timers, frames, the game, then the screen.
   Returns the time to wake next (ns), or 0 when the app called sys.exit. */
uint64_t jsrt_step(void);
int jsrt_exit_code(void);
/* NULL dir is the built-in picker. Pair start (even failed) with stop. */
void jsrt_stop(void);
/* JS.ELF driver: app exit -> picker, picker launch -> fresh app realm;
   picker exit -> 0 wake, return jsrt_exit_code() to the kernel. */
int jsapp_start(const char *dir, uint32_t heap_limit);
uint64_t jsapp_step(void);
/* Evaluates source as a script (the host tests' probes): 0, or -1 on an exception (logged). */
int jsrt_eval(const char *src, uint32_t n, const char *name);
