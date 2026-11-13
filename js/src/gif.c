/* SPDX-License-Identifier: MIT; GIF decoder (gif.h), a C port of Game-Maker:
 * Sandbox Edition's formats/gif.ts (decodeGif and lzwDecode, MIT), which
 * mirrors GIFC.C: LSB-first LZW codes from min size + 1 up to 12 bits, a local
 * color table replacing the global one, interlaced rows in four passes, data
 * that ends early leaving index 0. Any size of image is drawn through a
 * 320 x 200 window (a backdrop's offset), without decoding it whole. */
#include "gif.h"
/* Standalone import: gamemaker-os's fli.h supplies this cooperative checkpoint,
   which the JS runtime never hooks, so it is a no-op here (the FLI reader is not imported). */
#define gp_decode_checkpoint() ((void)0)
#define W 320u
#define H 200u
static int16_t prefix[4096];
static uint8_t suffix[4096], first[4096], stack[4097];
/* Where the decoded pixels go: the image's pixel (ix, iy) lands on the screen
   at (ix - sx, iy - sy) when that is inside 320 x 200 (drawImage). Rows come in
   file order, mapped through the four interlace passes when there are some. */
struct sink {uint8_t *px; int32_t sx, sy; uint32_t w, h, col, y; int pass, interlaced, done;};
static const uint8_t pass_start[4] = {0, 4, 2, 1}, pass_step[4] = {8, 8, 4, 2};
static void emit(struct sink *k, uint8_t v) {
  int64_t x = (int64_t)k->col - k->sx, y = (int64_t)k->y - k->sy;
  if (x >= 0 && x < (int32_t)W && y >= 0 && y < (int32_t)H) k->px[(uint32_t)y * W + (uint32_t)x] = v;
  if (++k->col < k->w) return;
  k->col = 0;
  if (!k->interlaced) {if (++k->y >= k->h || (int64_t)k->y - k->sy >= (int32_t)H) k->done = 1; return;}
  k->y += pass_step[k->pass];
  while (k->y >= k->h) {if (++k->pass >= 4) {k->done = 1; return;} k->y = pass_start[k->pass];}
}
static void lzw(const uint8_t *d, uint32_t n, uint32_t at, int min, struct sink *out, uint32_t count) {
  int clear = 1 << min, eoi = clear + 1, size = min + 1, next = clear + 2, prev = -1;
  uint32_t buf = 0, bits = 0, o = 0, left = 0; /* left: bytes left in the current sub-block */
  /* Every decode starts from the same table, as gif.ts's fresh arrays do: codes past
     clear are 0 until defined, so a stream naming an undefined code decodes the same
     whatever GIF came before (the board and the host decode in different orders). */
  for (int i = 0; i < 4096; i++) {prefix[i] = i < clear ? -1 : 0; suffix[i] = i < clear ? (uint8_t)i : 0; first[i] = i < clear ? (uint8_t)i : 0;}
  uint32_t work = 0;
  while (o < count && !out->done) {
    if (!(work++ & 255u)) gp_decode_checkpoint();
    while (bits < (uint32_t)size) {
      if (!left) {if (at >= n || !(left = d[at++])) return;}
      if (at >= n) return;
      buf |= (uint32_t)d[at++] << bits; bits += 8; left--;
    }
    int code = (int)(buf & ((1u << size) - 1)); buf >>= size; bits -= (uint32_t)size;
    if (code == clear) {size = min + 1; next = clear + 2; prev = -1; continue;}
    if (code == eoi) break;
    int c = code, sp = 0;
    if (prev >= 0 && code >= next) {stack[sp++] = first[prev]; c = prev;}
    while (c >= clear && sp < 4096) {stack[sp++] = suffix[c]; c = prefix[c];}
    stack[sp++] = (uint8_t)c;
    int head = c;
    while (sp > 0 && o < count && !out->done) {if (!(o & 1023u)) gp_decode_checkpoint(); emit(out, stack[--sp]); o++;}
    if (prev >= 0 && next < 4096) {
      prefix[next] = (int16_t)prev; suffix[next] = (uint8_t)head; first[next] = first[prev]; next++;
      if (next == 1 << size && size < 12) size++;
    }
    if (prev < 0) first[code] = (uint8_t)code;
    prev = code;
  }
}
static void colors(const uint8_t *d, uint32_t n, uint32_t *at, uint32_t count, uint8_t *pal) {
  for (uint32_t i = 0; i < 768; i++) pal[i] = 0;
  for (uint32_t i = 0; i < count * 3; i++) pal[i] = *at < n ? d[(*at)++] >> 2 : 0;
}
/* Reads up to the image data: the palette (a local table replaces the global
   one), the image's size and flags; the offset of the LZW minimum code size, or 0. */
static uint32_t head(const uint8_t *d, uint32_t n, uint8_t *pal, uint32_t *w, uint32_t *h, uint8_t *flags) {
  if (n < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F') return 0;
  uint32_t at = 10; uint8_t f = d[at++]; at += 2;
  if (f & 0x80) colors(d, n, &at, 2u << (f & 7), pal);
  else for (uint32_t i = 0; i < 768; i++) pal[i] = 0;
  for (;;) {
    if (at >= n) return 0;
    uint8_t b = d[at++];
    if (b == 0x21) {at++; while (at < n && d[at]) at += d[at] + 1u; at++;}
    else if (b == 0x2c) break;
    else return 0;
  }
  if (at + 9 > n) return 0;
  at += 4;
  *w = d[at] | d[at + 1] << 8; *h = d[at + 2] | d[at + 3] << 8; at += 4;
  f = d[at++];
  if (f & 0x80) colors(d, n, &at, 2u << (f & 7), pal);
  *flags = f;
  return at < n ? at : 0;
}
int gp_gif_palette(const uint8_t *d, uint32_t n, uint8_t *pal) {
  uint32_t w, h; uint8_t f;
  return head(d, n, pal, &w, &h, &f) ? 0 : -1;
}
int gp_gif_draw(const uint8_t *d, uint32_t n, int32_t sx, int32_t sy, uint8_t *pal, uint8_t *px) {
  uint32_t w = 0, h = 0; uint8_t flags = 0;
  uint32_t at = head(d, n, pal, &w, &h, &flags);
  if (!at) return -1;
  int min = d[at++];
  if (min < 2 || min > 8) return -1;
  /* the part of the image on the screen starts as index 0: data that ends early leaves it */
  for (int32_t y = 0; y < (int32_t)H; y++)
    for (int32_t x = 0; x < (int32_t)W; x++)
      if ((int64_t)x + sx >= 0 && (int64_t)x + sx < w && (int64_t)y + sy >= 0 && (int64_t)y + sy < h) px[(uint32_t)y * W + (uint32_t)x] = 0;
  if (!w || !h) return 0;
  struct sink k = {px, sx, sy, w, h, 0, 0, 0, (flags & 0x40) != 0, 0};
  lzw(d, n, at, min, &k, w * h);
  return 0;
}
int gp_gif(const uint8_t *d, uint32_t n, uint8_t *pal, uint8_t *px) {return gp_gif_draw(d, n, 0, 0, pal, px);}
