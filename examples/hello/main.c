/* SPDX-License-Identifier: MIT
 * HELLO: the smallest useful OS program for sandbox-kernel. It checks the ABI,
 * draws "Hello, world!" and a box on /dev/fb0 (960x540, 16-bit color), moves the box
 * with the arrow keys from /dev/input/event0, keeps the heartbeat, and exits on Esc.
 * Built only with sdk/ (see Makefile). Copy this folder to start your own OS. */
#include <sandbox/syscall.h>

#ifndef SBX_ABI_HAS_EINK
#define SBX_ABI_HAS_EINK 0  /* becomes 1 in the SDK once the e-ink API merges */
#endif

#define W 960u
#define H 540u
#define BOX 48u
#define STEP 16
#define BG 0x1082u    /* RGB565 dark blue */
#define FG 0xffffu    /* white text */
#define BOXC 0xfd20u  /* orange box */

static uint64_t log_fd, fb, input;
static uint8_t *pix;   /* mapped surface, as bytes: rows are `pitch` bytes apart, not W*2 */
static uint32_t pitch, width, height;

static void say(const char *s) { uint32_t n = 0; while (s[n]) n++; sbx_log(log_fd, s, n); }

/* A pixel rectangle, clipped to the screen. */
static void rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint16_t c) {
  for (int32_t j = y; j < y + (int32_t)h; j++) {
    if (j < 0 || j >= (int32_t)height) continue;
    uint16_t *row = (uint16_t *)(pix + (uint32_t)j * pitch);  /* pitch, not width */
    for (int32_t i = x; i < x + (int32_t)w; i++) if (i >= 0 && i < (int32_t)width) row[i] = c;
  }
}

/* Own 5x7 glyphs (MIT, like the rest of this file) for only the letters we draw.
   Each row is 5 bits, top row first. */
static const char glyph_chars[] = "Helo,wrd!";
static const uint8_t glyph_rows[][7] = {
  {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, {0x00,0x00,0x0e,0x11,0x1f,0x10,0x0e},  /* H e */
  {0x0c,0x04,0x04,0x04,0x04,0x04,0x0e}, {0x00,0x00,0x0e,0x11,0x11,0x11,0x0e},  /* l o */
  {0x00,0x00,0x00,0x00,0x0c,0x04,0x08}, {0x00,0x00,0x11,0x11,0x15,0x15,0x0a},  /* , w */
  {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, {0x01,0x01,0x0d,0x13,0x11,0x11,0x0f},  /* r d */
  {0x04,0x04,0x04,0x04,0x04,0x00,0x04},                                        /* ! */
};

/* Draw text scaled by `s`; characters we have no glyph for (the space) are blank. */
static void text(int32_t x, int32_t y, const char *t, uint32_t s) {
  for (; *t; t++, x += (int32_t)(6 * s))
    for (uint32_t g = 0; g < sizeof glyph_chars - 1; g++) {
      if (glyph_chars[g] != *t) continue;
      for (uint32_t r = 0; r < 7; r++) for (uint32_t c = 0; c < 5; c++)
        if (glyph_rows[g][r] >> (4 - c) & 1) rect(x + (int32_t)(c * s), y + (int32_t)(r * s), s, s, FG);
    }
}

static void draw(int32_t bx, int32_t by) {
  rect(0, 0, width, height, BG);
  text(120, 100, "Hello, world!", 10);
  rect(bx, by, BOX, BOX, BOXC);
}

int main(const sbx_startup *start) {
  /* 1. Refuse to run on a kernel older than the calls we use (clock 1.1, 960x540 mode 1.2). */
  if (start->major != 1 || start->minor < 2) return 2;
  if (sbx_open("/dev/log", 8, SBX_O_WRONLY, &log_fd)) return 3;
  say("HELLO start");

  /* 2. Framebuffer: ask for 960x540 in 16-bit color, then read back what we really got. */
  sbx_fb_mode mode;
  void *surface;
  if (sbx_open("/dev/fb0", 8, SBX_O_RDWR, &fb) || sbx_fb_set_mode(fb, W, H, SBX_FB_RGB565) ||
      sbx_fb_query(fb, &mode) || mode.format != SBX_FB_RGB565 ||
      !mode.width || !mode.height || mode.width > INT32_MAX || mode.height > INT32_MAX ||
      mode.pitch > UINT32_MAX || mode.pitch < mode.width * 2 || mode.buffer_bytes > UINT32_MAX ||
      mode.height > mode.buffer_bytes / mode.pitch ||
      sbx_fb_map(fb, mode.buffer_bytes, &surface)) { say("HELLO no usable screen"); return 4; }
  pix = surface; pitch = (uint32_t)mode.pitch;
  width = (uint32_t)mode.width; height = (uint32_t)mode.height;

  /* 3. Keyboard: read-only and nonblocking, so reads return -SBX_EAGAIN when nothing is queued. */
  if (sbx_open("/dev/input/event0", 17, SBX_O_RDONLY | SBX_O_NONBLOCK, &input)) return 6;

#if SBX_ABI_HAS_EINK
  /* E-ink goes here once the e-ink API merges (branch feature/eink-api); not built yet. */
#endif
  /* TODO(e-ink): open /dev/eink, set the title "HELLO OS" with a 40x40 mark, then write a page
     with a smiley. Left out on purpose: this kernel has no /dev/eink. See README.md. */

  int32_t bx = 400, by = 300;
  uint64_t seq = 1, now = 0;
  if (sbx_clock_ns(&now)) return 7;
  for (;;) {
    /* Heartbeat: the first call must come within 10 s of the start, then one every 2 s.
       The sequence starts at 1 and must increase. We call it every frame (about 30 ms). */
    if (sbx_health(start->generation, seq++)) return 5;

    /* Drain queued key events: type 1 is a key; value 1 is down, 2 is repeat, 0 is up. */
    for (;;) {
      sbx_input_event ev; uint64_t got = 0;
      int32_t status = sbx_read(input, &ev, sizeof ev, &got);
      if (status == -SBX_EAGAIN) break;
      if (status || got != sizeof ev) return 6;
      if (ev.type != SBX_EV_KEY || ev.value == 0) continue;
      if (ev.code == SBX_KEY_ESC) { say("HELLO bye"); return 0; }  /* returning from main exits */
      if (ev.code == SBX_KEY_LEFT) bx -= STEP;
      if (ev.code == SBX_KEY_RIGHT) bx += STEP;
      if (ev.code == SBX_KEY_UP) by -= STEP;
      if (ev.code == SBX_KEY_DOWN) by += STEP;
    }
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    int32_t max_x = width > BOX ? (int32_t)(width - BOX) : 0;
    int32_t max_y = height > BOX ? (int32_t)(height - BOX) : 0;
    if (bx > max_x) bx = max_x;
    if (by > max_y) by = max_y;

    draw(bx, by);
    if (sbx_fb_present(fb, 0, 0, width, height)) return 8;

    now += 33000000u;                /* next frame; sleeping lets the CPU rest */
    if (sbx_sleep_until(now)) return 7;
  }
}
