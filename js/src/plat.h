/* SPDX-License-Identifier: MIT; what the JS runtime needs from the machine it
 * runs on: the clock, files, the log, the 640x480 surface and input. The board
 * (plat_sbx.c, sandbox-kernel system calls) and the host tests
 * (tests/host/plat_host.c) each give one. */
#pragma once
/* Drop buffered events at an app/launcher boundary, without changing devices. */
void plat_reset_input(void);
#include <stdint.h>
#define PLAT_W 640u /* the mode a program starts in: 640x480, 32 bpp */
#define PLAT_H 480u
#define PLAT_SURFACE (640u * 480u * 4u) /* every mode fits in it (960x540 at 16 bpp does) */
uint64_t plat_clock_ns(void);           /* monotonic */
void plat_sleep_until(uint64_t ns);     /* returns early on input where it can */
void plat_heartbeat(void);              /* the kernel's health call; nothing on the host */
void plat_log(const char *s, uint32_t n);
/* Reads up to cap bytes of a file ("/sd/..." on the board); bytes read or -1. */
int32_t plat_read(const char *path, uint8_t *buf, uint32_t cap);
/* Lists a folder: calls fn for each entry (dir = 1 for folders); count or -1. */
int32_t plat_list(const char *path, void (*fn)(void *user, const char *name, uint32_t n, int dir, uint32_t size), void *user);
/* Files with a real handle (slice 4a). Each returns 0, or a negative errno (the kernel's numbers: -2 ENOENT, -16 EBUSY,
   -28 ENOSPC, -30 EROFS, -122 EDQUOT ...). plat_fread sets *got to 0 at the end of the file; plat_fseek takes an absolute
   position within [0, size] (the kernel has no other whence and no stat: sizes come from plat_list_meta). */
int plat_fopen(const char *path, uint64_t *fd);
int plat_fread(uint64_t fd, void *buf, uint32_t n, uint32_t *got);
int plat_fseek(uint64_t fd, uint32_t pos);
void plat_fclose(uint64_t fd);
/* The kernel's only write: open WRONLY|CREAT|TRUNC, write it all, fsync, close, on a canonical /sd/OS/CACHE/NAME or
   /sd/OS/SAVES/GAME/NAME (uppercase 8.3). Nothing reaches the card unless this returns 0. */
int plat_write_file(const char *path, const void *buf, uint32_t n);
/* A folder with sizes and dates (ABI 1.5 call 20, or call 11's 8.3 names with short_names). Count, or a negative errno. */
int32_t plat_list_meta(const char *path, int short_names,
                       void (*fn)(void *user, const char *name, uint32_t n, int dir, uint32_t size, uint32_t modified, uint32_t created), void *user);
/* Input device events the platform adds to the kernel's (type, code): the device came back or went away. A kernel
   SYN_DROPPED (type 0, code 3) comes through as it is, after the platform has read the state to empty the queue. */
#define PLAT_EV_ATTACH 0x100u
#define PLAT_EV_DETACH 0x101u
int plat_input_attached(void);
/* The best seed the machine has for Math.random: *fixed is set when it is a test's chosen seed (no live mixing). */
uint64_t plat_entropy(int *fixed);
void *plat_fb(void);                    /* the mode's pixels, rows of width x depth/8 bytes */
/* Another mode (the kernel's ABI 1.2; the firmware scales it to HDMI): depth
   32 (XRGB8888), 16 (RGB565) or 8 (palette indices). 0, or -1. The time the
   switch took goes to *us. */
int plat_set_mode(uint32_t w, uint32_t h, uint32_t depth, uint32_t *us);
int plat_palette(const uint8_t *pal); /* 256 entries, 6-bit VGA values */
void plat_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* The next input event (Linux type/code/value): 1, or 0 when none is waiting. */
int plat_event(uint16_t *type, uint16_t *code, int32_t *value);
/* Audio (slice 4c, ABI 1.8 /dev/audio): stereo S16 at 48000 frames a second, one writer, a 16384-frame ring. All return 0 or a
   negative errno (-16 EBUSY, -19 ENODEV when the kernel has no /dev/audio, -13 EACCES). plat_audio_write queues what fits and sets *taken
   (0 when the ring is full: the kernel's EAGAIN is not an error); it never blocks. plat_audio_close drops what is queued. */
struct plat_audio_info {uint64_t rate, capacity, queued, played, underruns, overruns, overrun_frames;};
int plat_audio_open(void);
int plat_audio_write(const int16_t *lr, uint32_t frames, uint32_t *taken);
int plat_audio_query(struct plat_audio_info *info);
void plat_audio_close(void);
/* E-ink (slice 4d, ABI 1.9 /dev/eink, docs/eink.md): one owner, write-only; a page is exactly 3904 bytes (250x122, 32-byte rows,
   bit 7 leftmost, 1 = black). All return 0 or a negative errno: -11 EAGAIN (too soon: the kernel changed nothing), -16 EBUSY, -19 ENODEV
   (no panel, or a kernel older than 1.9), -22 EINVAL, -9 EBADF. A close does not clear the panel; plat_eink_clear does. */
struct plat_eink_info {uint64_t width, height, page_bytes, title_max, mark_bytes, min_interval_ms, max_per_minute;};
int plat_eink_open(void);
int plat_eink_query(struct plat_eink_info *info);
int plat_eink_page(const uint8_t *page3904);
int plat_eink_title(const char *name, uint32_t name_bytes, const uint8_t *mark200, const char *build /* NUL-terminated or NULL */);
int plat_eink_clear(void);
void plat_eink_close(void);
/* Serial (slice 4d, ABI 1.10 /dev/serial0, docs/serial.md): opened nonblocking; one owner. plat_serial_read sets *got to 0 when
   nothing is waiting (the kernel's EAGAIN is not an error); plat_serial_write sets *taken (the kernel takes every byte and counts
   what does not fit as dropped; a host model may take fewer). */
struct plat_serial_info {uint64_t transport, tx_bytes, tx_dropped, rx_bytes, rx_dropped, tx_pending, rx_pending;};
int plat_serial_open(void);
int plat_serial_read(void *buf, uint32_t n, uint32_t *got);
int plat_serial_write(const void *buf, uint32_t n, uint32_t *taken);
int plat_serial_query(struct plat_serial_info *info);
void plat_serial_close(void);
/* Power (slice 4d, ABI 1.11 power_hold, 1.12 the asleep query). Flags: 1 = no blank, 2 = no slowdown. Acquire gives a token (-24 EMFILE
   when the 16-entry table is full, -22 EINVAL for no flags); release of a stale token is -9 EBADF. plat_power_asleep gives the
   flag bits of what the kernel has put to sleep now (bit 0 set: the screen is blank); -22 EINVAL on a kernel older than 1.12. */
int plat_power_acquire(uint32_t flags, uint64_t *token);
int plat_power_release(uint64_t token);
int plat_power_asleep(uint32_t *bits);
