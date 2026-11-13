/* SPDX-License-Identifier: MIT; plat.h on sandbox-kernel: its system calls
 * (ABI 1.1: the clock and sleep). The input device is opened again whenever
 * it goes away, as GMOS does. */
#include "plat.h"
#include <sandbox/syscall.h>
#include <string.h>
static uint64_t log_fd, fb_fd, in_fd;
static int have_input;
static uint32_t *fb, generation;
static uint64_t beat;
static sbx_input_event ev[16];
static uint32_t n, at;
int plat_sbx_init(const sbx_startup *start);
static void open_input(void) {
  sbx_input_state st;
  if (sbx_open("/dev/input/event0", 17, SBX_O_RDONLY | SBX_O_NONBLOCK, &in_fd)) return;
  have_input = 1;
  sbx_input_state_get(in_fd, &st); /* start from an empty queue */
}
int plat_sbx_init(const sbx_startup *start) {
  generation = start->generation;
  if (sbx_open("/dev/log", 8, SBX_O_WRONLY, &log_fd)) return 3;
  plat_heartbeat();
  sbx_fb_mode mode;
  if (sbx_open("/dev/fb0", 8, SBX_O_RDWR, &fb_fd) || sbx_fb_query(fb_fd, &mode) || mode.width != PLAT_W || mode.height != PLAT_H ||
      sbx_fb_map(fb_fd, mode.buffer_bytes, (void **)&fb)) return 4;
  open_input();
  return 0;
}
uint64_t plat_clock_ns(void) {uint64_t t = 0; sbx_clock_ns(&t); return t;}
void plat_sleep_until(uint64_t ns) {sbx_sleep_until(ns);}
void plat_heartbeat(void) {sbx_health(generation, ++beat);}
void plat_log(const char *s, uint32_t bytes) {sbx_log(log_fd, s, bytes > 120 ? 120 : bytes);}
int32_t plat_read(const char *path, uint8_t *buf, uint32_t cap) {
  uint64_t fd, got; uint32_t have = 0;
  if (sbx_open(path, (uint32_t)strlen(path), SBX_O_RDONLY, &fd)) return -1;
  while (have < cap && !sbx_read(fd, buf + have, cap - have > (1u << 20) ? 1u << 20 : cap - have, &got) && got) have += (uint32_t)got;
  sbx_close(fd);
  plat_heartbeat();
  return (int32_t)have;
}
int32_t plat_list(const char *path, void (*fn)(void *, const char *, uint32_t, int, uint32_t), void *user) {
  static uint8_t dents[4096] __attribute__((aligned(8)));
  uint64_t fd, got; int32_t count = 0;
  if (sbx_open(path, (uint32_t)strlen(path), SBX_O_RDONLY, &fd)) return -1;
  while (!sbx_getdents(fd, dents, sizeof dents, &got) && got) {
    for (uint32_t offset = 0; offset < got;) {
      const sbx_dirent *e = (const sbx_dirent *)(const void *)(dents + offset);
      if (e->length < sizeof *e || e->length > got - offset) break;
      fn(user, (const char *)(dents + offset + sizeof *e), e->name_bytes, e->type == SBX_DT_DIR, e->size);
      count++; offset += e->length;
    }
  }
  sbx_close(fd);
  return count;
}
void *plat_fb(void) {return fb;}
int plat_set_mode(uint32_t w, uint32_t h, uint32_t depth, uint32_t *us) {
  uint64_t t = plat_clock_ns();
  int r = sbx_fb_set_mode(fb_fd, w, h, depth == 32 ? SBX_FB_XRGB8888 : depth == 16 ? SBX_FB_RGB565 : SBX_FB_INDEX8);
  if (us) *us = (uint32_t)((plat_clock_ns() - t) / 1000u);
  return r ? -1 : 0;
}
int plat_palette(const uint8_t *pal) {
  static sbx_fb_palette_rec r;
  for (int i = 0; i < 256; i++) {
    uint32_t c[3]; for (int k = 0; k < 3; k++) {c[k] = pal[i * 3 + k] & 63; c[k] = c[k] << 2 | c[k] >> 4;}
    r.rgb[i] = c[0] << 16 | c[1] << 8 | c[2];
  }
  return sbx_fb_palette(fb_fd, &r, 0, 256) ? -1 : 0;
}
void plat_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {sbx_fb_present(fb_fd, x, y, w, h);}
int plat_event(uint16_t *type, uint16_t *code, int32_t *value) {
  if (at == n) {
    uint64_t got; at = n = 0;
    if (!have_input) {open_input(); if (!have_input) return 0;}
    int32_t s = sbx_read(in_fd, ev, sizeof ev, &got);
    if (s == -SBX_EAGAIN) return 0;
    if (s) {sbx_close(in_fd); have_input = 0; return 0;}
    n = (uint32_t)(got / sizeof ev[0]);
    if (!n) return 0;
  }
  *type = ev[at].type; *code = ev[at].code; *value = ev[at].value; at++;
  return 1;
}
void plat_reset_input(void) {
  at = n = 0;
  if (have_input) {sbx_input_state st; sbx_input_state_get(in_fd, &st);}
}
