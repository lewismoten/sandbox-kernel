/* SPDX-License-Identifier: MIT; the JS runtime on the host (plat.h over
 * stdio): "/sd" is a folder on disk (names matched without case, as the
 * card's 8.3 names are), the screen is memory, keys come from the command
 * line at given times, and the clock is real or virtual (--vclock: sleeping
 * jumps it, so tests are exact and fast).
 *   jsrun <sd folder> <app folder, e.g. /sd/OS/APPS/HELLO> [--ms N] [--vclock]
 *         [--keys ms:code:value,...] [--shot out.rgb] [--heap bytes] */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "jsrt.h"
#include "plat.h"

static const char *root;
static int vclock;
static uint64_t vnow;
static uint32_t fb[PLAT_SURFACE / 4];
static uint32_t mode_w = PLAT_W, mode_h = PLAT_H, mode_d = 32;
static uint8_t palette[768];
static struct {uint64_t at; uint16_t code; int32_t value;} keys[256];
static int nkeys, next_key;
static uint64_t start;

static uint64_t real_ns(void) {struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;}
uint64_t plat_clock_ns(void) {return vclock ? vnow : real_ns() - start;}
void plat_sleep_until(uint64_t ns) {
  if (vclock) {if (ns > vnow) vnow = ns; return;}
  uint64_t now = plat_clock_ns();
  if (ns > now) {struct timespec t = {(time_t)((ns - now) / 1000000000ull), (long)((ns - now) % 1000000000ull)}; nanosleep(&t, 0);}
}
void plat_heartbeat(void) {}
void plat_log(const char *s, uint32_t n) {printf("%.*s\n", (int)n, s); fflush(stdout);}
/* "/sd/A/B" to a path under root, each name matched without case. */
static int resolve(const char *path, char *out, size_t cap) {
  if (strncmp(path, "/sd", 3) || (path[3] && path[3] != '/')) return -1;
  snprintf(out, cap, "%s", root);
  for (const char *p = path + 3; *p;) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *e = p; while (*e && *e != '/') e++;
    char want[256]; size_t n = (size_t)(e - p);
    if (n >= sizeof want) return -1;
    memcpy(want, p, n); want[n] = 0;
    DIR *d = opendir(out); struct dirent *de; int found = 0;
    if (!d) return -1;
    while ((de = readdir(d))) if (!strcasecmp(de->d_name, want)) {size_t l = strlen(out); snprintf(out + l, cap - l, "/%s", de->d_name); found = 1; break;}
    closedir(d);
    if (!found) return -1;
    p = e;
  }
  return 0;
}
int32_t plat_read(const char *path, uint8_t *buf, uint32_t cap) {
  char real[1024];
  if (resolve(path, real, sizeof real)) return -1;
  FILE *f = fopen(real, "rb");
  if (!f) return -1;
  size_t n = fread(buf, 1, cap, f);
  fclose(f);
  return (int32_t)n;
}
int32_t plat_list(const char *path, void (*fn)(void *, const char *, uint32_t, int, uint32_t), void *user) {
  char real[1024];
  if (resolve(path, real, sizeof real)) return -1;
  DIR *d = opendir(real); struct dirent *de; int32_t n = 0;
  if (!d) return -1;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char up[256]; size_t i = 0;
    for (; de->d_name[i] && i < sizeof up - 1; i++) up[i] = (char)(de->d_name[i] >= 'a' && de->d_name[i] <= 'z' ? de->d_name[i] - 32 : de->d_name[i]);
    up[i] = 0; /* as the card lists them */
    fn(user, up, (uint32_t)i, de->d_type == DT_DIR, 0); n++;
  }
  closedir(d);
  return n;
}
void *plat_fb(void) {return fb;}
int plat_set_mode(uint32_t w, uint32_t h, uint32_t depth, uint32_t *us) {
  if ((depth != 8 && depth != 16 && depth != 32) || w * h * (depth / 8) > PLAT_SURFACE || w % 4) return -1;
  mode_w = w; mode_h = h; mode_d = depth; memset(fb, 0, sizeof fb);
  if (us) *us = 0;
  return 0;
}
int plat_palette(const uint8_t *pal) {memcpy(palette, pal, 768); return 0;}
void plat_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {(void)x; (void)y; (void)w; (void)h;}
int plat_event(uint16_t *type, uint16_t *code, int32_t *value) {
  if (next_key >= nkeys || keys[next_key].at > plat_clock_ns()) return 0;
  *type = 1; *code = keys[next_key].code; *value = keys[next_key].value; next_key++;
  return 1;
}
void plat_reset_input(void) {
  while (next_key < nkeys && keys[next_key].at <= plat_clock_ns()) next_key++;
}
/* Live heap blocks, for --cycles (a leak check): the system allocator's own count. */
#ifdef __APPLE__
#include <malloc/malloc.h>
static long blocks(void) {malloc_statistics_t st; malloc_zone_statistics(0, &st); return (long)st.blocks_in_use;}
#else
#include <malloc.h>
static long blocks(void) {struct mallinfo2 m = mallinfo2(); return (long)m.uordblks;}
#endif
int main(int argc, char **argv) {
  if (argc < 3) {fprintf(stderr, "usage: jsrun <sd folder> <app folder> [--ms N] [--vclock] [--keys ms:code:value,...] [--shot out.rgb] [--heap bytes]\n"); return 2;}
  root = argv[1];
  uint64_t ms = 1000; const char *shot = 0; uint32_t heap = 3u << 20; int launcher = 0; unsigned cycles = 0;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--ms") && i + 1 < argc) ms = strtoull(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--vclock")) vclock = 1;
    else if (!strcmp(argv[i], "--launcher")) launcher = 1;
    else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = (unsigned)strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--heap") && i + 1 < argc) heap = (uint32_t)strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
      for (char *p = argv[++i]; *p && nkeys < 256;) {
        unsigned long long at; unsigned code; int value, used;
        if (sscanf(p, "%llu:%u:%d%n", &at, &code, &value, &used) != 3) {fprintf(stderr, "bad --keys\n"); return 2;}
        keys[nkeys].at = at * 1000000ull; keys[nkeys].code = (uint16_t)code; keys[nkeys].value = value; nkeys++;
        p += used; if (*p == ',') p++;
      }
  }
  start = real_ns();
  if (cycles) { /* start, run for --ms and stop, again and again; prints the live heap blocks after the first and last */
    long first = 0;
    for (unsigned c = 0; c < cycles; c++) {
      vnow = 0; next_key = 0;
      if (jsrt_start(argv[2], heap)) {jsrt_stop(); printf("CYCLE %u start failed\n", c); return 1;}
      for (;;) {
        uint64_t wake = jsrt_step();
        if (!wake || plat_clock_ns() >= ms * 1000000ull) break;
        if (next_key < nkeys && keys[next_key].at < wake) wake = keys[next_key].at;
        plat_sleep_until(wake < ms * 1000000ull ? wake : ms * 1000000ull);
      }
      jsrt_stop();
      if (!c) first = blocks();
    }
    printf("CYCLES %u blocks first %ld last %ld\n", cycles, first, blocks());
    return 0;
  }
  const char *dir = !strcmp(argv[2], "--picker") ? 0 : argv[2];
  if (launcher ? jsapp_start(dir, heap) : jsrt_start(dir, heap)) {jsrt_stop(); return 1;}
  uint64_t end = ms * 1000000ull;
  for (;;) {
    uint64_t wake = launcher ? jsapp_step() : jsrt_step();
    if (!wake || plat_clock_ns() >= end) break;
    if (next_key < nkeys && keys[next_key].at < wake) wake = keys[next_key].at;
    plat_sleep_until(wake < end ? wake : end);
  }
  if (shot) { /* "GMJS" w h depth, the 768-byte palette, then the pixels (tools/jsshot.py makes a PNG) */
    FILE *f = fopen(shot, "wb");
    if (f) {uint32_t hd[4] = {0x534a4d47u, mode_w, mode_h, mode_d}; fwrite(hd, 4, 4, f); fwrite(palette, 1, 768, f); fwrite(fb, 1, mode_w * mode_h * (mode_d / 8), f); fclose(f);}
  }
  int status = jsrt_exit_code();
  jsrt_stop();
  return status;
}
