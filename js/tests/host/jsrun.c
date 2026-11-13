/* SPDX-License-Identifier: MIT; the JS runtime on the host (plat.h over
 * stdio): "/sd" is a folder on disk (names matched without case, as the
 * card's 8.3 names are), the screen is memory, keys come from the command
 * line at given times, and the clock is real or virtual (--vclock: sleeping
 * jumps it, so tests are exact and fast).
 *   jsrun <sd folder> <app folder, e.g. /sd/OS/APPS/HELLO> [--ms N] [--vclock]
 *         [--keys ms:code:value,...] [--shot out.rgb] [--heap bytes] */
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "handle.h"
#include "jsfs.h"
#include "jsrt.h"
#include "plat.h"

static const char *root;
static int vclock;
static uint64_t vnow;
static uint32_t fb[PLAT_SURFACE / 4];
static uint32_t mode_w = PLAT_W, mode_h = PLAT_H, mode_d = 32;
static uint8_t palette[768];
static struct {uint64_t at; uint16_t type, code; int32_t value;} keys[256];
static int host_attached = 1, have_seed, seed_live, sleepstat, devstat;
static uint64_t seed_value, max_sleep, sleeps;
static int nkeys, next_key;
static uint64_t start;

static uint64_t real_ns(void) {struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;}
uint64_t plat_clock_ns(void) {return vclock ? vnow : real_ns() - start;}
void plat_sleep_until(uint64_t ns) {
  if (sleepstat) {uint64_t was = plat_clock_ns(); if (ns > was && ns - was > max_sleep) max_sleep = ns - was; sleeps++;}
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
/* ---- files with handles, and the kernel's write rules (docs/saves-storage.md, core/sys.c) as a host model ---- */
static struct {FILE *f; int open, guarded; uint32_t size;} hf[16];
static int disk_full; /* --disk-full: every commit fails with ENOSPC */
static int guarded(const char *path) {return !strncasecmp(path, "/sd/OS/SAVES", 12) || !strncasecmp(path, "/sd/OS/CACHE", 12);}
int plat_fopen(const char *path, uint64_t *fd) {
  char real[1024]; struct stat st;
  if (resolve(path, real, sizeof real) || stat(real, &st)) return -2;
  for (unsigned i = 0; i < 16; i++) if (!hf[i].open) {
    FILE *f = fopen(real, "rb"); if (!f) return -13;
    hf[i].f = f; hf[i].open = 1; hf[i].guarded = guarded(path); hf[i].size = (uint32_t)st.st_size; *fd = i + 1;
    return 0;
  }
  return -24;
}
int plat_fread(uint64_t fd, void *buf, uint32_t n, uint32_t *got) {
  if (!fd || fd > 16 || !hf[fd - 1].open) return -9;
  *got = (uint32_t)fread(buf, 1, n, hf[fd - 1].f); return 0;
}
int plat_fseek(uint64_t fd, uint32_t pos) {
  if (!fd || fd > 16 || !hf[fd - 1].open) return -9;
  return pos > hf[fd - 1].size || fseek(hf[fd - 1].f, (long)pos, SEEK_SET) ? -22 : 0;
}
void plat_fclose(uint64_t fd) {if (fd && fd <= 16 && hf[fd - 1].open) {fclose(hf[fd - 1].f); hf[fd - 1].open = 0;}}
static int name_ok(const char *s, size_t n, int dot_ok) { /* fatput_name */
  size_t base = 0, ext = 0; int dot = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '.') {if (!dot_ok || dot || !base) return 0; dot = 1; continue;}
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    if (dot ? ++ext > 3 : ++base > 8) return 0;
  }
  return base && (!dot || ext);
}
static int count_entries(const char *dir, int want_dirs) {
  DIR *d = opendir(dir); struct dirent *de; int n = 0;
  if (!d) return 0;
  while ((de = readdir(d))) if (de->d_name[0] != '.' && (de->d_type == DT_DIR) == want_dirs) n++;
  closedir(d); return n;
}
int plat_write_file(const char *path, const void *buf, uint32_t n) {
  size_t len = strlen(path); const char *game = 0, *name; size_t glen = 0;
  if (!strncmp(path, "/sd/OS/CACHE/", 13)) {name = path + 13; if (len < 14 || len > 25 || !name_ok(name, len - 13, 1)) return -22;}
  else if (!strncmp(path, "/sd/OS/SAVES/", 13)) {
    game = path + 13; const char *sl = strchr(game, '/');
    if (!sl) return -22;
    glen = (size_t)(sl - game); name = sl + 1;
    if (!glen || glen > 8 || !*name || strlen(name) > 12 || !name_ok(game, glen, 0) || !name_ok(name, strlen(name), 1)) return -22;
  } else return -30;
  for (unsigned i = 0; i < 16; i++) if (hf[i].open && hf[i].guarded) return -16; /* a reader open in the save area blocks writes */
  if (disk_full || n > JSFS_KERNEL_MAX) return -28;
  char dir[1024], real[2200], os[1024];
  snprintf(os, sizeof os, "%s/OS", root); mkdir(os, 0777);
  snprintf(dir, sizeof dir, "%s/OS/%s", root, game ? "SAVES" : "CACHE"); mkdir(dir, 0777);
  if (game) {
    char gd[1100]; struct stat st;
    snprintf(gd, sizeof gd, "%s/%.*s", dir, (int)glen, game);
    if (stat(gd, &st)) {if (count_entries(dir, 1) >= 64) return -122; mkdir(gd, 0777);}
    snprintf(real, sizeof real, "%s/%s", gd, name);
    if (stat(real, &st) && count_entries(gd, 0) >= 32) return -122;
  } else snprintf(real, sizeof real, "%s/%s", dir, name);
  FILE *f = fopen(real, "wb"); if (!f) return -5;
  int bad = n && fwrite(buf, 1, n, f) != n;
  if (fclose(f) || bad) return -5;
  return 0;
}
int32_t plat_list_meta(const char *path, int short_names,
                       void (*fn)(void *, const char *, uint32_t, int, uint32_t, uint32_t, uint32_t), void *user) {
  char real[1024];
  if (resolve(path, real, sizeof real)) return -2;
  DIR *d = opendir(real); struct dirent *de; int32_t n = 0;
  if (!d) return -2;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char full[1400], up[256]; struct stat st; snprintf(full, sizeof full, "%s/%s", real, de->d_name);
    if (stat(full, &st)) continue;
    snprintf(up, sizeof up, "%s", de->d_name);
    if (short_names) for (char *c = up; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    fn(user, up, (uint32_t)strlen(up), S_ISDIR(st.st_mode), (uint32_t)st.st_size, 0x5a21u, 0x5a21u); n++;
  }
  closedir(d);
  return n;
}
void *plat_fb(void) {return fb;}
int plat_set_mode(uint32_t w, uint32_t h, uint32_t depth, uint32_t *us) {
  /* the kernel's rule: width 16 to 1920 and a multiple of 4, height 16 to 1200, at most 1,228,800 bytes */
  if ((depth != 8 && depth != 16 && depth != 32) || w < 16 || w > 1920 || h < 16 || h > 1200 || w * h * (depth / 8) > PLAT_SURFACE || w % 4) return -1;
  mode_w = w; mode_h = h; mode_d = depth; memset(fb, 0, sizeof fb);
  if (us) *us = 0;
  return 0;
}
int plat_palette(const uint8_t *pal) {memcpy(palette, pal, 768); return 0;}
void plat_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {(void)x; (void)y; (void)w; (void)h;}
int plat_event(uint16_t *type, uint16_t *code, int32_t *value) {
  if (next_key >= nkeys || keys[next_key].at > plat_clock_ns()) return 0;
  *type = keys[next_key].type; *code = keys[next_key].code; *value = keys[next_key].value; next_key++;
  if (*type == PLAT_EV_ATTACH) host_attached = 1; else if (*type == PLAT_EV_DETACH) host_attached = 0;
  return 1;
}
int plat_input_attached(void) {return host_attached;}
uint64_t plat_entropy(int *fixed) {
  static uint64_t n;
  *fixed = have_seed && !seed_live;
  if (have_seed) return seed_value;
  return real_ns() * 0x9e3779b97f4a7c15ull ^ (uint64_t)getpid() << 32 ^ ++n;
}
void plat_reset_input(void) {
  while (next_key < nkeys && keys[next_key].at <= plat_clock_ns()) next_key++;
}
/* The kernel's 48 kHz audio ring (docs/audio-device.md): 16384 frames, whole frames, never blocks (full is a short count or 0),
   drained by the virtual clock at 48 frames a millisecond; a millisecond that finds the ring short counts one underrun, but only
   after the first write of an open. --no-audio: no /dev/audio (ENODEV); --audio-busy: another program holds it (EBUSY). */
#define AU_CAP 16384u
static int pw_dark(void);
static int au_open, au_busy, au_none, au_first;
static FILE *au_dump; /* --audio-dump file: every frame the ring accepted, as the kernel would get it (S16 stereo LE) */
static uint64_t au_dark_frames, au_last, au_queued, au_played, au_under, au_over, au_over_frames;
static void au_drain(void) {
  uint64_t now = plat_clock_ns(), ms = (now - au_last) / 1000000ull;
  if (!ms) return;
  au_last += ms * 1000000ull;
  if (!au_first) return;
  uint64_t due = ms * 48u;
  if (au_queued >= due) {au_queued -= due; au_played += due; return;}
  au_under += ms - au_queued / 48u; au_played += au_queued; au_queued = 0;
}
int plat_audio_open(void) {
  if (au_none) return -19;
  if (au_busy || au_open) return -16;
  au_open = 1; au_first = 0; au_queued = au_played = au_under = au_over = au_over_frames = 0; au_last = plat_clock_ns(); return 0;
}
int plat_audio_write(const int16_t *lr, uint32_t frames, uint32_t *taken) {
  (void)lr; *taken = 0;
  if (!au_open) return -9;
  au_drain(); au_first = 1;
  uint64_t room = AU_CAP - au_queued, n = frames < room ? frames : room;
  if (n < frames) {au_over++; au_over_frames += frames - n;}
  if (pw_dark()) {au_dark_frames += n; *taken = (uint32_t)n; return 0;} /* the kernel accepts and drops audio while the screen is blank */
  if (au_dump) {fwrite(lr, 4, n, au_dump); fflush(au_dump);}
  au_queued += n; *taken = (uint32_t)n; return 0;
}
int plat_audio_query(struct plat_audio_info *o) {
  if (!au_open) return -9;
  au_drain();
  *o = (struct plat_audio_info){48000, AU_CAP, au_queued, au_played, au_under, au_over, au_over_frames}; return 0;
}
void plat_audio_close(void) {au_open = 0; au_queued = 0;}

/* E-ink model (docs/eink.md): one write-only owner, a 3904-byte page; an update is accepted at least 1000 ms after the last accepted one
   and at most 30 in any 60 s, otherwise -11 (EAGAIN) with nothing changed. --no-eink: no panel (ENODEV); --eink-busy: someone holds it (EBUSY);
   --eink-dump FILE: one record per accepted update (u32 kind 0 page 1 clear 2 title, u32 virtual ms, 3904 bytes of the page). */
static unsigned ei_min_ms = 1000;
static int ei_has_last, ei_open, ei_busy, ei_none, ei_opens, ei_acc_n, ei_eagain, ei_clears;
static uint64_t ei_last, ei_stamps[64]; static unsigned ei_ns;
static FILE *ei_dump;
static int ei_gate(void) {
  uint64_t now = plat_clock_ns(), win = 60000000000ull; unsigned in = 0;
  if (!ei_open) return -9;
  for (unsigned i = 0; i < ei_ns; i++) if (now < ei_stamps[i] + win) in++;
  if ((ei_has_last && now < ei_last + ei_min_ms * 1000000ull) || in >= 30) {ei_eagain++; return -11;}
  unsigned k = 0;
  for (unsigned i = 0; i < ei_ns; i++) if (now < ei_stamps[i] + win) ei_stamps[k++] = ei_stamps[i];
  ei_stamps[k++] = now; ei_ns = k; ei_last = now; ei_has_last = 1; ei_acc_n++; return 0;
}
static void ei_rec(uint32_t kind, const uint8_t *page) {
  static const uint8_t zero[3904]; uint32_t h[2] = {kind, (uint32_t)(plat_clock_ns() / 1000000ull)};
  if (ei_dump) {fwrite(h, 4, 2, ei_dump); fwrite(page ? page : zero, 1, 3904, ei_dump); fflush(ei_dump);}
}
int plat_eink_open(void) {
  if (ei_none) return -19;
  if (ei_busy || ei_open) return -16;
  ei_open = 1; ei_opens++; ei_has_last = 0; ei_ns = 0; return 0;
}
int plat_eink_query(struct plat_eink_info *o) {
  if (!ei_open) return -9;
  *o = (struct plat_eink_info){250, 122, 3904, 16, 200, 1000, 30}; return 0;
}
int plat_eink_page(const uint8_t *p) {int r = ei_gate(); if (!r) ei_rec(0, p); return r;}
int plat_eink_title(const char *name, uint32_t nb, const uint8_t *mark, const char *build) {
  (void)name; (void)nb; (void)mark; (void)build; int r = ei_gate(); if (!r) ei_rec(2, 0); return r;
}
int plat_eink_clear(void) {int r = ei_gate(); if (!r) {ei_clears++; ei_rec(1, 0);} return r;}
void plat_eink_close(void) {ei_open = 0;}
/* Serial model (docs/serial.md): nonblocking, one owner. --serial-rx at:hex,at:hex puts bytes in a 256-byte receive buffer at those
   virtual ms (what does not fit is dropped and counted); read gives what is waiting up to the request (partial). A 256-byte transmit buffer
   drains 64 bytes a millisecond; write takes what fits (the real kernel takes all and counts the excess dropped: a model of partial
   writes, see G20); --serial-tx-stuck stops the drain. --serial-dump FILE gets every accepted byte. --no-serial: ENODEV. */
#define SE_CAP 256u
static int se_open, se_none, se_stuck; static unsigned se_opens;
static struct {uint64_t at; uint8_t b[64]; unsigned n;} se_in[64]; static unsigned se_nin, se_next;
static uint8_t se_rx[SE_CAP]; static unsigned se_rxn;
static uint64_t se_txp, se_tx_last, se_txb, se_txd, se_rxb, se_rxd;
static FILE *se_dump;
static void se_sync(void) {
  uint64_t now = plat_clock_ns();
  while (se_next < se_nin && se_in[se_next].at <= now) {
    for (unsigned i = 0; i < se_in[se_next].n; i++) {
      if (se_rxn < SE_CAP) {se_rx[se_rxn++] = se_in[se_next].b[i]; se_rxb++;} else se_rxd++;
    }
    se_next++;
  }
  uint64_t ms = (now - se_tx_last) / 1000000ull;
  if (ms) {se_tx_last += ms * 1000000ull; if (!se_stuck) {uint64_t d = ms * 64u; se_txp = d >= se_txp ? 0 : se_txp - d;}}
}
int plat_serial_open(void) {
  if (se_none) return -19;
  if (se_open) return -16;
  se_open = 1; se_opens++; se_tx_last = plat_clock_ns(); return 0;
}
int plat_serial_read(void *buf, uint32_t n, uint32_t *got) {
  *got = 0;
  if (!se_open) return -9;
  se_sync();
  uint32_t k = n < se_rxn ? n : se_rxn;
  memcpy(buf, se_rx, k); memmove(se_rx, se_rx + k, se_rxn - k); se_rxn -= k; *got = k; return 0;
}
int plat_serial_write(const void *buf, uint32_t n, uint32_t *taken) {
  *taken = 0;
  if (!se_open) return -9;
  se_sync();
  uint32_t room = SE_CAP - (uint32_t)se_txp, k = n < room ? n : room;
  if (se_dump) {fwrite(buf, 1, k, se_dump); fflush(se_dump);}
  se_txp += k; se_txb += k; *taken = k; return 0;
}
int plat_serial_query(struct plat_serial_info *o) {
  if (!se_open) return -9;
  se_sync();
  *o = (struct plat_serial_info){1, se_txb, se_txd, se_rxb, se_rxd, se_txp, se_rxn}; return 0;
}
void plat_serial_close(void) {se_open = 0; se_rxn = 0; se_txp = 0;}
/* Power model (docs/power-saving.md): a 16-entry token table (ids never repeat: 1000 up), flags 1 and 2 only, a stale or unknown release is -9,
   a full table -24. --blank a:b,c:d: the screen is blank from virtual ms a to b ... unless a no-blank hold exists (a hold taken while blank
   restores the screen at once). The query (1.12) gives bit 0 while blank; --old-power: the query is -22 (a kernel older than 1.12). */
static struct {uint64_t tok; uint32_t flags;} pw[16]; static unsigned pw_n; static uint64_t pw_next = 1000;
static unsigned pw_acq, pw_rel, pw_queries; static int pw_old;
static struct {uint64_t a, b;} pw_blank[16]; static unsigned pw_nb;
int plat_power_acquire(uint32_t flags, uint64_t *token) {
  if (!flags || flags & ~3u) return -22;
  if (pw_n == 16) return -24;
  pw[pw_n].tok = pw_next; pw[pw_n].flags = flags; *token = pw_next++; pw_n++; pw_acq++; return 0;
}
int plat_power_release(uint64_t token) {
  for (unsigned i = 0; i < pw_n; i++) if (pw[i].tok == token) {pw[i] = pw[--pw_n]; pw_rel++; return 0;}
  return -9;
}
int plat_power_asleep(uint32_t *bits) {
  uint64_t now = plat_clock_ns(); uint32_t held = 0; pw_queries++;
  if (pw_old) return -22;
  for (unsigned i = 0; i < pw_n; i++) held |= pw[i].flags;
  *bits = 0;
  for (unsigned i = 0; i < pw_nb; i++) if (now >= pw_blank[i].a * 1000000ull && now < pw_blank[i].b * 1000000ull && !(held & 1u)) *bits = 1;
  return 0;
}
/* Whether the model screen is blank, for the audio model: audio written while blank is accepted and dropped. */
static int pw_dark(void) {uint32_t b = 0; unsigned q = pw_queries; plat_power_asleep(&b); pw_queries = q; return b & 1u;}
static void print_devstat(void) { /* after stop: what the app left behind, and what the models saw */
  printf("HOST dev eink_open %d serial_open %d audio_open %d holds %u acquired %u released %u eink_accepted %d eink_eagain %d eink_clears %d serial_opens %u eink_opens %d dark_audio_frames %llu\n",
         ei_open, se_open, au_open, pw_n, pw_acq, pw_rel, ei_acc_n, ei_eagain, ei_clears, se_opens, ei_opens, (unsigned long long)au_dark_frames);
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
  uint64_t ms = 1000; const char *shot = 0; uint32_t heap = 3u << 20; int launcher = 0, autorun = 0; unsigned cycles = 0; const char *alt = 0;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--ms") && i + 1 < argc) ms = strtoull(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--vclock")) vclock = 1;
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {have_seed = 1; seed_value = strtoull(argv[++i], 0, 10);} /* Math.random starts from this, as a test */
    else if (!strcmp(argv[i], "--seed-live") && i + 1 < argc) {have_seed = seed_live = 1; seed_value = strtoull(argv[++i], 0, 10);} /* the same, but key timings are still mixed in (a test of that) */
    else if (!strcmp(argv[i], "--sleepstat")) sleepstat = 1; /* at the end: HOST sleeps N max_us M (the longest single sleep the loop asked for) */
    else if (!strcmp(argv[i], "--no-audio")) au_none = 1;
    else if (!strcmp(argv[i], "--audio-dump") && i + 1 < argc) au_dump = fopen(argv[++i], "wb");
    else if (!strcmp(argv[i], "--audio-busy")) au_busy = 1;
    else if (!strcmp(argv[i], "--no-eink")) ei_none = 1;
    else if (!strcmp(argv[i], "--eink-busy")) ei_busy = 1;
    else if (!strcmp(argv[i], "--eink-dump") && i + 1 < argc) ei_dump = fopen(argv[++i], "wb");
    else if (!strcmp(argv[i], "--eink-min-ms") && i + 1 < argc) ei_min_ms = (unsigned)strtoul(argv[++i], 0, 10); /* the kernel enforces more than it reports */
    else if (!strcmp(argv[i], "--no-serial")) se_none = 1;
    else if (!strcmp(argv[i], "--serial-dump") && i + 1 < argc) se_dump = fopen(argv[++i], "wb");
    else if (!strcmp(argv[i], "--serial-tx-stuck")) se_stuck = 1;
    else if (!strcmp(argv[i], "--serial-rx") && i + 1 < argc) { /* at_ms:hexbytes,... (up to 64 bytes each, in order) */
      for (char *p = argv[++i]; *p && se_nin < 64;) {
        unsigned long long at; int used = 0;
        if (sscanf(p, "%llu:%n", &at, &used) < 1 || !used) {fprintf(stderr, "bad --serial-rx\n"); return 2;}
        p += used; se_in[se_nin].at = at * 1000000ull; se_in[se_nin].n = 0;
        while (isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) && se_in[se_nin].n < 64) {
          unsigned v; sscanf(p, "%2x", &v); se_in[se_nin].b[se_in[se_nin].n++] = (uint8_t)v; p += 2;
        }
        se_nin++; if (*p == ',') p++;
      }
    }
    else if (!strcmp(argv[i], "--blank") && i + 1 < argc) {
      for (char *p = argv[++i]; *p && pw_nb < 16;) {
        unsigned long long a, b; int used = 0;
        if (sscanf(p, "%llu:%llu%n", &a, &b, &used) != 2) {fprintf(stderr, "bad --blank\n"); return 2;}
        pw_blank[pw_nb].a = a; pw_blank[pw_nb].b = b; pw_nb++; p += used; if (*p == ',') p++;
      }
    }
    else if (!strcmp(argv[i], "--old-power")) pw_old = 1;
    else if (!strcmp(argv[i], "--devstat")) devstat = 1;
    else if (!strcmp(argv[i], "--launcher")) launcher = 1;
    else if (!strcmp(argv[i], "--autorun")) autorun = launcher = 1; /* start as JS.ELF does: from /sd/OS/AUTORUN.TXT (argv[2] is ignored) */
    else if (!strcmp(argv[i], "--strict")) jsrt_require_manifest = 1;
    else if (!strcmp(argv[i], "--write-quota") && i + 1 < argc) jsrt_write_quota = (uint32_t)strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--disk-full")) disk_full = 1;
    else if (!strcmp(argv[i], "--alt") && i + 1 < argc) alt = argv[++i]; /* with --cycles: alternate between argv[2] and this app */
    else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = (unsigned)strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--heap") && i + 1 < argc) heap = (uint32_t)strtoul(argv[++i], 0, 10);
    else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
      for (char *p = argv[++i]; *p && nkeys < 256;) {
        unsigned long long at; unsigned code, type = 1; int value, used;
        if (sscanf(p, "%llu:%u:%d%n", &at, &code, &value, &used) != 3) {fprintf(stderr, "bad --keys\n"); return 2;}
        p += used; if (*p == ':' && sscanf(p, ":%u%n", &type, &used) == 1) p += used; /* ms:code:value[:type] (type 0 code 3 = SYN_DROPPED, 256 attach, 257 detach) */
        keys[nkeys].at = at * 1000000ull; keys[nkeys].type = (uint16_t)type; keys[nkeys].code = (uint16_t)code; keys[nkeys].value = value; nkeys++;
        if (*p == ',') p++;
      }
  }
  start = real_ns();
  if (cycles) { /* start, run for --ms and stop, again and again; prints the live heap blocks after the first and last */
    long first = 0; unsigned leaks = 0;
    for (unsigned c = 0; c < cycles; c++) {
      vnow = 0; next_key = 0;
      if (jsrt_start(alt && c % 2 ? alt : argv[2], heap)) {jsrt_stop(); printf("CYCLE %u start failed\n", c); return 1;}
      for (;;) {
        uint64_t wake = jsrt_step();
        if (!wake || plat_clock_ns() >= ms * 1000000ull) break;
        if (next_key < nkeys && keys[next_key].at < wake) wake = keys[next_key].at;
        plat_sleep_until(wake < ms * 1000000ull ? wake : ms * 1000000ull);
      }
      leaks += (unsigned)jsrt_stop();
      if (!c) first = blocks();
    }
    if (devstat) print_devstat();
    printf("CYCLES %u blocks first %ld last %ld leaks %u open %u\n", cycles, first, blocks(), leaks, jsrt_handles_open());
    return 0;
  }
  const char *dir = !strcmp(argv[2], "--picker") ? 0 : argv[2];
  if (autorun ? jsapp_autorun(heap) : launcher ? jsapp_start(dir, heap) : jsrt_start(dir, heap)) {jsrt_stop(); return 1;}
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
  if (devstat) print_devstat();
  if (sleepstat) printf("HOST sleeps %llu max_us %llu\n", (unsigned long long)sleeps, (unsigned long long)(max_sleep / 1000u));
  return status;
}
