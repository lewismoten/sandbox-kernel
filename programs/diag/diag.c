/* SPDX-License-Identifier: MIT; the first OS program for sandbox-kernel,
 * built only against sdk/. It logs, reports readiness, checks a few ABI rules from the user
 * side, draws a tile pattern through /dev/fb0 (960x540 at 16 bpp with ABI 1.2,
 * so a 1080p screen shows it doubled and edge to edge; else 640x480x32),
 * reads /sd/MIT.TXT and exits. */
#include <sandbox/syscall.h>
static uint64_t log_fd;
static unsigned length(const char *s) {unsigned n = 0; while (s[n]) n++; return n;}
static char *put(char *p, const char *s) {while (*s) *p++ = *s++; *p = 0; return p;}
static char *num(char *p, uint32_t n) {
  char t[11]; int k = 0;
  do t[k++] = (char)('0' + n % 10); while (n /= 10);
  while (k) *p++ = t[--k];
  *p = 0; return p;
}
static void say(const char *s) {sbx_log(log_fd, s, length(s));}
static uint32_t crc_add(uint32_t crc, const uint8_t *p, uint32_t n) {
  while (n--) {crc ^= *p++; for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));}
  return crc;
}
/* Square tiles (s pixels: 60 at 960x540, 80 at 640x480), color from the
   tile's place, a darker checker and a white frame. */
static uint32_t tile(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t s) {
  if (x < 8 || y < 8 || x >= w - 8 || y >= h - 8) return 0xffffff;
  uint32_t kx = x / s, ky = y / s, tx = kx * 8 / (w / s), ty = ky * 6 / (h / s); /* colors spread over 8 x 6 steps, as at 640x480 */
  uint32_t r = 40 + tx * 26, g = 40 + ty * 34, b = 230 - tx * 12;
  if ((kx + ky) & 1) {r /= 2; g /= 2; b /= 2;}
  return r << 16 | g << 8 | b;
}
/* ABI rules seen from User mode; the count is logged. */
#define CHECKS 11
static int checks(void) {
  int ok = 0; uint64_t fd, n;
  sbx_close_rec bad = {{sizeof bad, 0, 1, 0}, 0};
  ok += sbx_close(0x12345) == -SBX_EBADF;                                        /* never-issued handle */
  ok += sbx_open((const char *)0x00100000, 8, SBX_O_RDONLY, &fd) == -SBX_EFAULT; /* kernel address */
  ok += sbx_syscall(9, &bad, sizeof bad) == -SBX_ENOSYS;                         /* unknown call */
  ok += sbx_syscall(SBX_CALL_CLOSE, &bad, sizeof bad) == -SBX_EINVAL;            /* nonzero reserved */
  if (!sbx_open("/sd/MIT.TXT", 11, SBX_O_RDONLY, &fd)) {
    ok += sbx_read(fd, (void *)0x00100000, 16, &n) == -SBX_EFAULT;               /* kernel buffer */
    ok += sbx_read(fd, (void *)(uintptr_t)&checks, 16, &n) == -SBX_EFAULT;      /* read-only text */
    ok += sbx_read(fd, &n, 1u << 21, &n) == -SBX_EINVAL;                         /* over 1 MiB */
    ok += sbx_ioctl(fd, SBX_IOC_FB_QUERY, &n, 8) == -SBX_ENOTTY;                 /* not a framebuffer */
    sbx_close(fd);
    ok += sbx_close(fd) == -SBX_EBADF;                                           /* stale handle */
  }
  ok += sbx_open("/sd/NOPE.TXT", 12, SBX_O_RDONLY, &fd) == -SBX_ENOENT;
  ok += sbx_open("/sd/MIT.TXT", 11, SBX_O_WRONLY, &fd) == -SBX_EROFS;
  return ok;
}
int main(const sbx_startup *start) {
  char line[96], *p;
  if (sbx_open("/dev/log", 8, SBX_O_WRONLY, &log_fd)) return 3;
  p = put(line, "DIAG hello, ABI "); p = num(p, start->major); p = put(p, "."); p = num(p, start->minor);
  p = put(p, ", generation "); p = num(p, start->generation); p = put(p, start->trial ? ", trial" : ""); say(line);
  say(sbx_health(start->generation, 1) ? "DIAG health refused" : "DIAG ready (health 1)");
  p = put(line, "DIAG checks "); p = num(p, (uint32_t)checks()); p = put(p, "/"); p = num(p, CHECKS); say(line);
  uint64_t fb; sbx_fb_mode mode; void *surface;
  if (sbx_open("/dev/fb0", 8, SBX_O_RDWR, &fb) || sbx_fb_query(fb, &mode) || sbx_fb_map(fb, mode.buffer_bytes, &surface)) say("DIAG no framebuffer");
  else {
    /* 16:9 when the kernel has screen modes (ABI 1.2); the surface mapped above holds either */
    int wide = start->minor >= 2 && !sbx_fb_set_mode(fb, 960, 540, SBX_FB_RGB565) && !sbx_fb_query(fb, &mode) && mode.format == SBX_FB_RGB565;
    uint32_t w = (uint32_t)mode.width, h = (uint32_t)mode.height, bytes = w * h * (wide ? 2u : 4u), crc = ~0u;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
      uint32_t c = tile(x, y, w, h, wide ? 60 : 80);
      if (wide) ((uint16_t *)surface)[y * w + x] = (uint16_t)((c >> 8 & 0xf800u) | (c >> 5 & 0x07e0u) | (c >> 3 & 0x001fu));
      else ((uint32_t *)surface)[y * w + x] = c;
    }
    int s = sbx_fb_present(fb, 0, 0, w, h);
    crc = ~crc_add(crc, (const uint8_t *)surface, bytes);
    p = put(line, "DIAG fb "); p = num(p, w); p = put(p, "x"); p = num(p, h); p = put(p, wide ? "x16" : "x32");
    p = put(p, s ? " present failed" : " presented, crc "); if (!s) num(p, crc);
    say(line);
  }
  uint64_t fd, got; static uint8_t chunk[1000];
  if (sbx_open("/sd/MIT.TXT", 11, SBX_O_RDONLY, &fd)) say("DIAG /sd/MIT.TXT missing");
  else {
    uint32_t total = 0, crc = ~0u; int s;
    while (!(s = sbx_read(fd, chunk, sizeof chunk, &got)) && got) {crc = crc_add(crc, chunk, (uint32_t)got); total += (uint32_t)got;}
    sbx_close(fd);
    p = put(line, "DIAG /sd/MIT.TXT "); p = num(p, total); p = put(p, " bytes crc "); p = num(p, ~crc); if (s) put(p, " read error");
    say(line);
  }
  say("DIAG done, exit 0");
  return 0;
}
