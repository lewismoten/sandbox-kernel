/* SPDX-License-Identifier: MIT
 * CLOCK: shows the uptime ("MM:SS") from sbx_clock_ns, once a second, waiting with
 * sbx_sleep_until. Same skeleton as ../hello; Esc exits. */
#include <sandbox/syscall.h>
#define W 960u
#define H 540u
static uint64_t log_fd, fb;
static uint8_t *pix;
static uint32_t pitch;
/* 5x7 digits, rows top first, 5 bits each (own bitmaps, MIT). */
static const uint8_t dig[11][7] = {
  {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {31,2,4,2,1,17,14}, {2,6,10,18,31,2,2},
  {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
  {0,12,12,0,12,12,0}, /* index 10 is ':' */
};
static void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) {
  for (uint32_t j = y; j < y + h && j < H; j++) {
    uint16_t *row = (uint16_t *)(pix + j * pitch);
    for (uint32_t i = x; i < x + w && i < W; i++) row[i] = c;
  }
}
static void glyph(uint32_t x, uint32_t y, uint32_t g, uint32_t s) {
  for (uint32_t r = 0; r < 7; r++) for (uint32_t c = 0; c < 5; c++)
    if (dig[g][r] >> (4 - c) & 1) rect(x + c * s, y + r * s, s, s, 0xffff);
}
int main(const sbx_startup *start) {
  if (start->major != 1 || start->minor < 2) return 2;
  if (sbx_open("/dev/log", 8, SBX_O_WRONLY, &log_fd)) return 3;
  sbx_fb_mode mode; void *surface; uint64_t input;
  if (sbx_open("/dev/fb0", 8, SBX_O_RDWR, &fb) || sbx_fb_set_mode(fb, W, H, SBX_FB_RGB565) || sbx_fb_query(fb, &mode) ||
      mode.format != SBX_FB_RGB565 || sbx_fb_map(fb, mode.buffer_bytes, &surface)) return 4;
  pix = surface; pitch = (uint32_t)mode.pitch;
  sbx_open("/dev/input/event0", 17, SBX_O_RDONLY | SBX_O_NONBLOCK, &input);
  uint64_t seq = 1, t0, now;
  sbx_clock_ns(&t0);
  for (;;) {
    if (sbx_health(start->generation, seq++)) return 5;  /* once a second is well inside the 2 s limit */
    sbx_clock_ns(&now);
    uint32_t sec = (uint32_t)((now - t0) / 1000000000u), m = sec / 60 % 100, s = sec % 60;
    uint32_t d[5] = {m / 10, m % 10, 10, s / 10, s % 10};
    rect(0, 0, W, H, 0x0010);
    for (uint32_t i = 0; i < 5; i++) glyph(130 + i * 140, 200, d[i], 20);
    sbx_fb_present(fb, 0, 0, W, H);
    sbx_input_event ev; uint64_t got;
    while (!sbx_read(input, &ev, sizeof ev, &got) && got == sizeof ev)
      if (ev.type == SBX_EV_KEY && ev.value == 1 && ev.code == SBX_KEY_ESC) return 0;
    sbx_sleep_until(now + 1000000000u);
  }
}
