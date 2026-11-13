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
static uint64_t next_try, startup_mix;
int plat_sbx_init(const sbx_startup *start);
static void open_input(void) {
  sbx_input_state st;
  if (sbx_open("/dev/input/event0", 17, SBX_O_RDONLY | SBX_O_NONBLOCK, &in_fd)) return;
  have_input = 1;
  sbx_input_state_get(in_fd, &st); /* start from an empty queue */
}
int plat_sbx_init(const sbx_startup *start) {
  generation = start->generation;
  startup_mix = start->generation * 0x9e3779b97f4a7c15ull ^ start->trial ^ (uint64_t)start->image_bytes << 20;
  if (sbx_open("/dev/log", 8, SBX_O_WRONLY, &log_fd)) return 3;
  plat_heartbeat();
  sbx_fb_mode mode;
  if (sbx_open("/dev/fb0", 8, SBX_O_RDWR, &fb_fd) || sbx_fb_query(fb_fd, &mode) || mode.width != PLAT_W || mode.height != PLAT_H ||
      sbx_fb_map(fb_fd, mode.buffer_bytes, (void **)&fb)) return 4;
  open_input();
  return 0;
}
uint64_t plat_clock_ns(void) {uint64_t t = 0; sbx_clock_ns(&t); return t;}
/* The kernel offers no random source (contract gap G7): mix the clock at app start (the SD and USB timing before it varies a
   little), the startup record, and the low bits of 64 back-to-back clock reads (timer jitter, a few bits at best). Not for secrets. */
uint64_t plat_entropy(int *fixed) {
  uint64_t h = startup_mix ^ plat_clock_ns() * 0xff51afd7ed558ccdull, prev = 0, t;
  *fixed = 0;
  for (int i = 0; i < 64; i++) {sbx_clock_ns(&t); h = (h ^ (t - prev) ^ t << 17) * 0xc4ceb9fe1a85ec53ull; h ^= h >> 31; prev = t;}
  return h;
}
int plat_input_attached(void) {return have_input;}
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
int plat_fopen(const char *path, uint64_t *fd) {return sbx_open(path, (uint32_t)strlen(path), SBX_O_RDONLY, fd);}
int plat_fread(uint64_t fd, void *buf, uint32_t count, uint32_t *got) {
  uint64_t d = 0; int32_t s = sbx_read(fd, buf, count > (1u << 20) ? 1u << 20 : count, &d);
  *got = s ? 0 : (uint32_t)d; plat_heartbeat();
  return s;
}
int plat_fseek(uint64_t fd, uint32_t pos) {uint64_t where; return sbx_seek(fd, (int64_t)pos, &where);}
void plat_fclose(uint64_t fd) {sbx_close(fd);}
int plat_write_file(const char *path, const void *buf, uint32_t bytes) {
  uint64_t fd, done; int32_t s = sbx_open(path, (uint32_t)strlen(path), SBX_O_WRONLY | SBX_O_CREAT | SBX_O_TRUNC, &fd);
  if (s) return s;
  for (uint32_t off = 0; !s && off < bytes;) {
    uint32_t chunk = bytes - off > 65536u ? 65536u : bytes - off;
    s = sbx_write(fd, (const uint8_t *)buf + off, chunk, &done);
    if (!s && !done) s = -SBX_EIO;
    if (!s) off += (uint32_t)done;
    plat_heartbeat();
  }
  if (!s) s = sbx_fsync(fd); /* the commit; a close without it discards the bytes */
  sbx_close(fd);
  plat_heartbeat();
  return s;
}
int32_t plat_list_meta(const char *path, int short_names,
                       void (*fn)(void *, const char *, uint32_t, int, uint32_t, uint32_t, uint32_t), void *user) {
  static uint8_t dents[4096] __attribute__((aligned(8)));
  uint64_t fd, got; int32_t count = 0, s = sbx_open(path, (uint32_t)strlen(path), SBX_O_RDONLY, &fd);
  if (s) return s;
  while (!(s = short_names ? sbx_getdents_meta(fd, dents, sizeof dents, &got) : sbx_getdents_long_meta(fd, dents, sizeof dents, &got)) && got) {
    for (uint32_t offset = 0; offset < got;) {
      const sbx_dirent_meta *e = (const sbx_dirent_meta *)(const void *)(dents + offset);
      if (e->length < sizeof *e || e->length > got - offset) break;
      fn(user, (const char *)(dents + offset + sizeof *e), e->name_bytes, e->type == SBX_DT_DIR, e->size, e->modified, e->created);
      count++; offset += e->length;
    }
  }
  sbx_close(fd);
  return s ? s : count;
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
    if (!have_input) { /* try again at most every 100 ms: a reopen costs a system call */
      uint64_t now = plat_clock_ns();
      if (now < next_try) return 0;
      next_try = now + 100000000ull;
      open_input();
      if (!have_input) return 0;
      *type = PLAT_EV_ATTACH; *code = 0; *value = 0; return 1;
    }
    int32_t s = sbx_read(in_fd, ev, sizeof ev, &got);
    if (s == -SBX_EAGAIN) return 0;
    if (s) {sbx_close(in_fd); have_input = 0; next_try = plat_clock_ns() + 100000000ull; *type = PLAT_EV_DETACH; *code = 0; *value = 0; return 1;}
    n = (uint32_t)(got / sizeof ev[0]);
    if (!n) return 0;
  }
  *type = ev[at].type; *code = ev[at].code; *value = ev[at].value; at++;
  if (*type == SBX_EV_SYN && *code == SBX_SYN_DROPPED) { /* lost events: read the state (it empties the queue) and drop what we hold */
    sbx_input_state st; sbx_input_state_get(in_fd, &st); at = n = 0;
  }
  return 1;
}
void plat_reset_input(void) {
  at = n = 0;
  if (have_input) {sbx_input_state st; sbx_input_state_get(in_fd, &st);}
}
/* Audio: /dev/audio is opened for the one writer and closed again at stop (ABI 1.8; EAGAIN from a full ring is a short count). */
static uint64_t au_fd; static int au_open;
int plat_audio_open(void) {
  if (au_open) return -SBX_EBUSY;
  int32_t s = sbx_open("/dev/audio", 10, SBX_O_WRONLY, &au_fd);
  if (s == -SBX_ENOENT) return -19; /* a kernel older than 1.8 has no /dev/audio */
  if (s) return s;
  au_open = 1; return 0;
}
int plat_audio_write(const int16_t *lr, uint32_t frames, uint32_t *taken) {
  uint64_t done = 0; *taken = 0;
  if (!au_open) return -SBX_EBADF;
  int32_t s = sbx_write(au_fd, lr, frames * 4u, &done);
  if (s == -SBX_EAGAIN) return 0;
  if (s) return s;
  *taken = (uint32_t)(done / 4u); plat_heartbeat(); return 0;
}
int plat_audio_query(struct plat_audio_info *o) {
  sbx_audio_info i; int32_t s = au_open ? sbx_audio_query(au_fd, &i) : -SBX_EBADF;
  if (s) return s;
  o->rate = i.rate; o->capacity = i.capacity_frames; o->queued = i.queued_frames; o->played = i.played_frames;
  o->underruns = i.underruns; o->overruns = i.overruns; o->overrun_frames = i.overrun_frames; return 0;
}
void plat_audio_close(void) {if (au_open) {sbx_close(au_fd); au_open = 0;}}
/* E-ink, serial and power (slice 4d): thin wrappers; a missing device or a kernel older than the ABI that has it is -19 ENODEV. */
static uint64_t ei_fd, se_fd; static int ei_open, se_open;
int plat_eink_open(void) {
  if (ei_open) return -SBX_EBUSY;
  int32_t s = sbx_open("/dev/eink", 9, SBX_O_WRONLY, &ei_fd);
  if (s == -SBX_ENOENT) return -SBX_ENODEV;
  if (s) return s;
  ei_open = 1; return 0;
}
int plat_eink_query(struct plat_eink_info *o) {
  sbx_eink_info i; int32_t s = ei_open ? sbx_eink_query(ei_fd, &i) : -SBX_EBADF;
  if (s) return s;
  o->width = i.width; o->height = i.height; o->page_bytes = i.page_bytes; o->title_max = i.title_max; o->mark_bytes = i.mark_bytes;
  o->min_interval_ms = i.min_interval_ms; o->max_per_minute = i.max_per_minute; return 0;
}
int plat_eink_page(const uint8_t *p) {return ei_open ? sbx_eink_page(ei_fd, p) : -SBX_EBADF;}
int plat_eink_title(const char *name, uint32_t nb, const uint8_t *mark, const char *build) {
  sbx_eink_title_rec r;
  return ei_open ? sbx_eink_title(ei_fd, &r, name, nb, mark, build) : -SBX_EBADF;
}
int plat_eink_clear(void) {return ei_open ? sbx_eink_clear(ei_fd) : -SBX_EBADF;}
void plat_eink_close(void) {if (ei_open) {sbx_close(ei_fd); ei_open = 0;}}
int plat_serial_open(void) {
  if (se_open) return -SBX_EBUSY;
  int32_t s = sbx_open("/dev/serial0", 12, SBX_O_RDWR | SBX_O_NONBLOCK, &se_fd);
  if (s == -SBX_ENOENT) return -SBX_ENODEV;
  if (s) return s;
  se_open = 1; return 0;
}
int plat_serial_read(void *buf, uint32_t cap, uint32_t *got) {
  uint64_t d = 0; *got = 0;
  if (!se_open) return -SBX_EBADF;
  int32_t s = sbx_read(se_fd, buf, cap, &d);
  if (s == -SBX_EAGAIN) return 0;
  if (s) return s;
  *got = (uint32_t)d; return 0;
}
int plat_serial_write(const void *buf, uint32_t len, uint32_t *taken) {
  uint64_t d = 0; *taken = 0;
  if (!se_open) return -SBX_EBADF;
  int32_t s = sbx_write(se_fd, buf, len, &d);
  if (s == -SBX_EAGAIN) return 0;
  if (s) return s;
  *taken = (uint32_t)d; return 0;
}
int plat_serial_query(struct plat_serial_info *o) {
  sbx_serial_info i; int32_t s = se_open ? sbx_serial_query(se_fd, &i) : -SBX_EBADF;
  if (s) return s;
  o->transport = i.transport; o->tx_bytes = i.tx_bytes; o->tx_dropped = i.tx_dropped; o->rx_bytes = i.rx_bytes;
  o->rx_dropped = i.rx_dropped; o->tx_pending = i.tx_pending; o->rx_pending = i.rx_pending; return 0;
}
void plat_serial_close(void) {if (se_open) {sbx_close(se_fd); se_open = 0;}}
int plat_power_acquire(uint32_t flags, uint64_t *token) {return sbx_power_acquire(flags, token);}
int plat_power_release(uint64_t token) {return sbx_power_release(token);}
int plat_power_asleep(uint32_t *bits) {return sbx_power_asleep(bits);}
