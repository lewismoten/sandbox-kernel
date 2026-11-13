/* SPDX-License-Identifier: MIT; the JS runtime: QuickJS-ng with the
 * browser-like API of prelude.js over the element tree (dom.h), the game
 * element (the Game-Maker play core through session.h), files, timers and
 * frames on the platform's clock (plat.h). One app at a time. */
#pragma once
#include <stdint.h>
/* Limits (apps.md, "Limits"): the JS heap an app may use (the board's malloc pool is 3 MiB, shared with the element tree and
   the runtime), the milliseconds of script one loop turn or one start may run, and QuickJS's own stack depth. */
#define JSRT_HEAP_LIMIT (1536u << 10)
#define JSRT_STEP_BUDGET_MS 500u
#define JSRT_START_BUDGET_MS 1000u
#define JSRT_JS_STACK (256u * 1024u)
#define JSRT_DOM_DEPTH 64u    /* elements nest at most this deep: layout, paint and teardown recurse on the 1 MiB stack */
#define JSRT_EXIT_TIMEOUT 124 /* an app stopped for running too long, as timeout(1) says it */
#define JSRT_EXIT_OOM 137     /* ... or for running out of memory */
#define JSRT_VERSION "0.3.0" /* the runtime (slice 3); the API an app asks for is APP_API_VERSION in app.h */
struct jsrt_stats {
  uint64_t start_ns;                  /* runtime and context made, prelude and app run */
  uint64_t js_ns, render_ns, present_ns; /* time in JS, in layout and paint, presenting (totals) */
  uint32_t frames, presents, input_max; /* input_max: the most input events handled in one turn */
  uint32_t game_ticks, game_late_max_us, game_step_max_us;
  uint64_t game_play_ns;              /* time in the play phase (ticks are due only then) */
};
extern struct jsrt_stats jsrt_stats;
#include <stddef.h>
/* The JS heap in use and its high-water mark since the last start, and live blocks (0 after jsrt_stop). */
size_t jsrt_heap_used(void);
size_t jsrt_heap_peak(void);
size_t jsrt_heap_blocks(void);
/* Starts the app in folder dir ("/sd/OS/APPS/HELLO": runs its MAIN.JS);
   0, or -1 (the reason is logged). heap_limit: bytes for the JS heap. */
int jsrt_start(const char *dir, uint32_t heap_limit);
/* One turn of the loop: input, timers, frames, the game, then the screen.
   Returns the time to wake next (ns), or 0 when the app called sys.exit. */
uint64_t jsrt_step(void);
int jsrt_exit_code(void);
/* NULL dir is the built-in picker. Pair start (even failed) with stop. Stop releases every handle the app
   left open and returns how many (logged: "JS app left N handle(s) open: file 2"). */
int jsrt_stop(void);
/* 1: an app folder with no APP.JSON is refused (default 0: it runs as a legacy app with every capability). */
extern int jsrt_require_manifest;
/* JS.ELF's start: reads /sd/OS/AUTORUN.TXT (line 2: APPS/NAME or PICKER; none means APPS/HELLO); a bad line or an app
   that does not start falls back to the picker, with the reason on screen. 0, or -1 when even the picker failed. */
int jsapp_autorun(uint32_t heap_limit);
/* JS.ELF driver: app exit -> picker, picker launch -> fresh app realm;
   picker exit -> 0 wake, return jsrt_exit_code() to the kernel. */
int jsapp_start(const char *dir, uint32_t heap_limit);
uint64_t jsapp_step(void);
/* Evaluates source as a script (the host tests' probes): 0, or -1 on an exception (logged). */
int jsrt_eval(const char *src, uint32_t n, const char *name);
