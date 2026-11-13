/* SPDX-License-Identifier: MIT; a test-only profile: logs every hook the core calls ("PT ..."),
 * holds one counted allocation per playing element, and draws a test picture, so
 * tests/profile_run.py can prove the order of init/play/key/step/end/stop and that stop
 * releases everything. Built only into build/host/jsrun-pt. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "profile.h"

static unsigned allocs, frees, steps, draws;
static unsigned char *held;            /* the element's session, one allocation owned by a core handle (handle.h) */
static uint32_t held_id;
static int leaky;                      /* a session "LEAKY" never closes its handle in end(): the core must report it at stop */
static unsigned char pic[JSRT_PIC_W * JSRT_PIC_H], pal[768];
static void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char *fmt, ...) {char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); jsrt_log(b);}

static void t_init(void) {steps = draws = 0; logf("PT init");}
static void t_stop(void) {logf("PT stop open=%u allocs=%u frees=%u", jsrt_handles_open(), allocs, frees);} /* the core's count: 0 unless a session leaked */
static void t_release(void *u) {free(u); frees++; if (u == held) {held = 0; held_id = 0;}}
static int t_play(const char *dir, void **h, char *err, uint32_t cap) {
  if (strstr(dir, "BAD")) {snprintf(err, cap, "no game in %s", dir); logf("PT play-refused %s", dir); return -1;}
  held = malloc(1024); allocs++; memset(held, 7, 1024);
  leaky = strstr(dir, "LEAKY") != 0;
  held_id = jsrt_handle_open(JSRT_H_GAME, t_release, held);
  if (!held_id) {free(held); held = 0; snprintf(err, cap, "too many open handles"); return -1;}
  *h = held; steps = draws = 0;
  logf("PT play %s", dir);
  if (strstr(dir, "READ")) { /* a hook reads through the app's path policy, not plat_read */
    uint8_t b[16]; char p[96]; snprintf(p, sizeof p, "%s/GAME.GAM", dir);
    logf("PT read %s %d", p, (int)jsrt_fs_read(p, b, sizeof b));
    snprintf(p, sizeof p, "/sd/OS/AUTORUN.TXT");
    logf("PT read %s %d", p, (int)jsrt_fs_read(p, b, sizeof b));
  }
  return 0;
}
static void t_end(void) {logf("PT end"); if (held_id && !leaky) jsrt_handle_close(held_id);}
static int t_key(uint16_t code, int32_t value) {logf("PT key %u %d", code, (int)value); return code == 1 && value == 1 ? JSRT_END : 0;}
static uint32_t t_step(uint64_t now, struct jsrt_direct *d) {
  (void)now; steps++;
  if (steps % 4) return 0;
  for (unsigned i = 0; i < sizeof pic; i++) pic[i] = (unsigned char)(i + steps);
  for (unsigned i = 0; i < 768; i++) pal[i] = (unsigned char)(i / 12);
  d->px = pic; d->pal = pal; return JSRT_DRAW;
}
static uint64_t t_wake(uint64_t now) {return now + 5000000ull;}
static void t_draw(void *h, uint32_t *fb, uint32_t stride) {
  if (h != held) logf("PT draw-wrong-handle");
  if (!draws++) logf("PT draw");
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) fb[y * stride + x] = 0xff00ff00u; (void)stride;
}
static uint32_t t_ticks(void) {return steps;}
static int t_picture(const char *src, uint8_t *px, uint8_t *p) {
  if (!strstr(src, "PIC")) return -1;
  logf("PT picture %s", src);
  memset(px, 3, JSRT_PIC_W * JSRT_PIC_H); memset(p, 20, 768); return 0;
}
static const struct jsrt_profile test = {JSRT_PROFILE_ABI, "test", t_init, t_stop, t_play, t_end, t_key, t_step, t_wake, t_draw, t_ticks, t_picture};
const struct jsrt_profile *const jsrt_profile = &test;
