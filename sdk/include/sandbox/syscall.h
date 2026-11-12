/* SPDX-License-Identifier: MIT
 * sandbox-kernel SDK: raw ARM32 system-call wrappers (ARM state, soft-float).
 * svc #0 with r7 = call, r0 = record, r1 = record bytes, r2 = ABI major,
 * r3-r6 = 0. Each wrapper returns 0 or a negative SBX_E* value; a libc layer
 * can turn that into -1 and errno later. */
#ifndef SANDBOX_SYSCALL_H
#define SANDBOX_SYSCALL_H
#include "abi.h"
#ifdef HOST_TEST
/* Test-owned transport only; production always uses the ARM svc below. */
int32_t sbx_syscall(uint32_t call, void *record, uint32_t bytes);
#else
#if !defined(__arm__) || defined(__thumb__)
#error "sandbox/syscall.h targets ARM-state ARM32 (build with -marm)"
#endif

__attribute__((always_inline)) static inline int32_t sbx_syscall(uint32_t call, void *record, uint32_t bytes) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)record;
  register uint32_t r1 __asm__("r1") = bytes;
  register uint32_t r2 __asm__("r2") = SBX_ABI_MAJOR;
  register uint32_t r3 __asm__("r3") = 0;
  register uint32_t r4 __asm__("r4") = 0;
  register uint32_t r5 __asm__("r5") = 0;
  register uint32_t r6 __asm__("r6") = 0;
  register uint32_t r7 __asm__("r7") = call;
  __asm__ volatile("svc #0"
                   : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                   : "r"(r4), "r"(r5), "r"(r6), "r"(r7)
                   : "r12", "lr", "memory", "cc");
  return (int32_t)r0;
}
#endif

#define SBX_HEADER(type) {sizeof(type), 0, 0, 0}
/* Unavailable startup or ENOSYS means disable this optional feature; no mmap
   fallback. Errors leave both caller outputs untouched. */
static inline int32_t sbx_app_arena(const sbx_startup *startup, uint64_t requested,
                                   void **address, uint64_t *actual_bytes) {
  uint64_t epoch = sbx_app_generation(startup);
  if (!epoch) return -SBX_ENOSYS;
  sbx_app_arena_rec r = {SBX_HEADER(sbx_app_arena_rec), epoch, requested, 0, 0};
  int32_t s = sbx_syscall(SBX_CALL_APP_ARENA, &r, sizeof r);
  if (!s) { *address = (void *)(uintptr_t)r.address; *actual_bytes = r.actual_bytes; }
  return s;
}

static inline int32_t sbx_open(const char *path, uint32_t path_bytes, uint32_t flags, uint64_t *fd) {
  sbx_open_rec r = {SBX_HEADER(sbx_open_rec), (uint32_t)(uintptr_t)path, path_bytes, flags, 0, 0};
  int32_t s = sbx_syscall(SBX_CALL_OPEN, &r, sizeof r);
  if (!s) *fd = r.fd;
  return s;
}
static inline int32_t sbx_read(uint64_t fd, void *buffer, uint32_t count, uint64_t *done) {
  sbx_io_rec r = {SBX_HEADER(sbx_io_rec), fd, (uint32_t)(uintptr_t)buffer, count, 0};
  int32_t s = sbx_syscall(SBX_CALL_READ, &r, sizeof r);
  if (!s) *done = r.transferred;
  return s;
}
static inline int32_t sbx_write(uint64_t fd, const void *buffer, uint32_t count, uint64_t *done) {
  sbx_io_rec r = {SBX_HEADER(sbx_io_rec), fd, (uint32_t)(uintptr_t)buffer, count, 0};
  int32_t s = sbx_syscall(SBX_CALL_WRITE, &r, sizeof r);
  if (!s) *done = r.transferred;
  return s;
}
static inline int32_t sbx_close(uint64_t fd) {
  sbx_close_rec r = {SBX_HEADER(sbx_close_rec), fd};
  return sbx_syscall(SBX_CALL_CLOSE, &r, sizeof r);
}
/* Cache writes: one writer, <=1 MiB RAM, fsync commits by COW. Close/reset
   abort unflushed bytes, never implicit sync. */
static inline int32_t sbx_fsync(uint64_t fd) {
  sbx_close_rec r = {SBX_HEADER(sbx_close_rec), fd};
  return sbx_syscall(SBX_CALL_FSYNC, &r, sizeof r);
}
/* Cache or one save folder: copy/publish rename (same folder only); temp may survive interrupted cleanup.
   Destination is old/new, no shared chains. Close the writer first. */
static inline int32_t sbx_rename(const char *from, uint32_t from_bytes, const char *to, uint32_t to_bytes) {
  sbx_rename_rec r = {SBX_HEADER(sbx_rename_rec), (uint32_t)(uintptr_t)from, from_bytes, (uint32_t)(uintptr_t)to, to_bytes};
  return sbx_syscall(SBX_CALL_RENAME, &r, sizeof r);
}
/* ABI 1.3: absolute [0,file size], including EOF. Failure preserves position. */
static inline int32_t sbx_seek(uint64_t fd, int64_t offset, uint64_t *position) {
  sbx_seek_rec r = {SBX_HEADER(sbx_seek_rec), fd, offset, SBX_SEEK_SET, 0};
  int32_t s = sbx_syscall(SBX_CALL_SEEK, &r, sizeof r);
  if (!s) *position = r.position;
  return s;
}
static inline int32_t sbx_ioctl(uint64_t fd, uint32_t request, void *record, uint32_t bytes) {
  sbx_ioctl_rec r = {SBX_HEADER(sbx_ioctl_rec), fd, request, (uint32_t)(uintptr_t)record, bytes};
  return sbx_syscall(SBX_CALL_IOCTL, &r, sizeof r);
}
/* Log one line: fd is an open handle on /dev/log. */
static inline int32_t sbx_log(uint64_t log_fd, const char *text, uint32_t bytes) {
  uint64_t done;
  return sbx_write(log_fd, text, bytes, &done);
}
static inline int32_t sbx_fb_query(uint64_t fb_fd, sbx_fb_mode *mode) {
  *mode = (sbx_fb_mode){SBX_HEADER(sbx_fb_mode), 0, 0, 0, 0, 0};
  return sbx_ioctl(fb_fd, SBX_IOC_FB_QUERY, mode, sizeof *mode);
}
static inline int32_t sbx_fb_present(uint64_t fb_fd, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  sbx_fb_present_rec p = {SBX_HEADER(sbx_fb_present_rec), x, y, w, h};
  return sbx_ioctl(fb_fd, SBX_IOC_FB_PRESENT, &p, sizeof p);
}
/* ABI 1.2: another screen mode (SBX_FB_*), then query it again for its pitch. */
static inline int32_t sbx_fb_set_mode(uint64_t fb_fd, uint32_t w, uint32_t h, uint32_t format) {
  sbx_fb_set_rec r = {SBX_HEADER(sbx_fb_set_rec), w, h, format};
  return sbx_ioctl(fb_fd, SBX_IOC_FB_SET_MODE, &r, sizeof r);
}
/* ABI 1.2: palette entries for SBX_FB_INDEX8 (r is filled from rgb). */
static inline int32_t sbx_fb_palette(uint64_t fb_fd, sbx_fb_palette_rec *r, uint32_t first, uint32_t count) {
  r->h = (sbx_header)SBX_HEADER(sbx_fb_palette_rec); r->first = first; r->count = count;
  return sbx_ioctl(fb_fd, SBX_IOC_FB_PALETTE, r, sizeof *r);
}
/* ABI 1.9: /dev/eink (open it write-only). Limits and rules: docs/eink.md; -SBX_EAGAIN means too soon. */
static inline int32_t sbx_eink_query(uint64_t fd, sbx_eink_info *info) {
  *info = (sbx_eink_info){SBX_HEADER(sbx_eink_info), 0, 0, 0, 0, 0, 0, 0, 0};
  return sbx_ioctl(fd, SBX_IOC_EINK_QUERY, info, sizeof *info);
}
/* name: 1-16 characters; mark: 200 bytes (40x40) or NULL; build: the line under the name, free text
 * (a hash, "v0.4.2", a date, or "v0.4 3A9F2C1"), NUL-terminated, or NULL for a blank line. Longer than 16
 * characters is clipped here. r is scratch for the record. */
static inline int32_t sbx_eink_title(uint64_t fd, sbx_eink_title_rec *r, const char *name, uint32_t name_bytes, const uint8_t *mark, const char *build) {
  *r = (sbx_eink_title_rec){SBX_HEADER(sbx_eink_title_rec), mark ? SBX_EINK_TITLE_MARK : 0, name_bytes, {0}, {0}, 0, {0}};
  if (build) { uint32_t n = 0; while (n < 16 && build[n]) { r->build[n] = build[n]; n++; } r->build_bytes = n; }
  for (uint32_t i = 0; i < name_bytes && i < 16; i++) r->name[i] = name[i];
  if (mark) for (uint32_t i = 0; i < 200; i++) r->mark[i] = mark[i];
  return sbx_ioctl(fd, SBX_IOC_EINK_TITLE, r, sizeof *r);
}
static inline int32_t sbx_eink_clear(uint64_t fd) {
  sbx_header h = SBX_HEADER(sbx_header);
  return sbx_ioctl(fd, SBX_IOC_EINK_CLEAR, &h, sizeof h);
}
/* A whole page: SBX_EINK_PAGE_BYTES bytes. */
static inline int32_t sbx_eink_page(uint64_t fd, const uint8_t *bits) {
  uint64_t done;
  return sbx_write(fd, bits, SBX_EINK_PAGE_BYTES, &done);
}
/* Map the open /dev/fb0 surface (read/write, shared); *address gets its start. */
static inline int32_t sbx_fb_map(uint64_t fb_fd, uint64_t bytes, void **address) {
  sbx_mmap_rec r = {SBX_HEADER(sbx_mmap_rec), fb_fd, 0, bytes, SBX_PROT_READ | SBX_PROT_WRITE, SBX_MAP_SHARED, 0};
  int32_t s = sbx_syscall(SBX_CALL_MMAP, &r, sizeof r);
  if (!s) *address = (void *)(uintptr_t)r.address;
  return s;
}
/* Directory entries (sbx_dirent) of an open /sd directory; *done 0 at the end. */
static inline int32_t sbx_getdents(uint64_t dir_fd, void *buffer, uint32_t bytes, uint64_t *done) {
  sbx_dirents_rec r = {SBX_HEADER(sbx_dirents_rec), dir_fd, (uint32_t)(uintptr_t)buffer, bytes, 0};
  int32_t s = sbx_syscall(SBX_CALL_GETDENTS, &r, sizeof r);
  if (!s) *done = r.transferred;
  return s;
}
static inline int32_t sbx_getdents_meta(uint64_t dir_fd, void *buffer, uint32_t bytes, uint64_t *done) {
  sbx_dirents_rec r = {SBX_HEADER(sbx_dirents_rec), dir_fd, (uint32_t)(uintptr_t)buffer, bytes, 0};
  int32_t s = sbx_syscall(SBX_CALL_GETDENTS_META, &r, sizeof r);
  if (!s) *done = r.transferred;
  return s;
}
/* ABI 1.5: sbx_dirent_meta with validated VFAT UTF-8 names (or short aliases).
 * At least 280 bytes fits any one entry; old calls 11/18 retain short names. */
static inline int32_t sbx_getdents_long_meta(uint64_t dir_fd, void *buffer, uint32_t bytes, uint64_t *done) {
  sbx_dirents_rec r = {SBX_HEADER(sbx_dirents_rec), dir_fd, (uint32_t)(uintptr_t)buffer, bytes, 0};
  int32_t s = sbx_syscall(SBX_CALL_GETDENTS_LONG_META, &r, sizeof r);
  if (!s) *done = r.transferred;
  return s;
}
/* Heartbeat: call within 10 s of starting (readiness), then at least every 2 s. */
static inline int32_t sbx_health(uint32_t generation, uint64_t sequence) {
  sbx_health_rec r = {SBX_HEADER(sbx_health_rec), generation, sequence};
  return sbx_syscall(SBX_CALL_HEALTH, &r, sizeof r);
}
/* Monotonic nanoseconds since the kernel started (ABI 1.1). */
static inline int32_t sbx_clock_ns(uint64_t *ns) {
  sbx_clock_rec r = {SBX_HEADER(sbx_clock_rec), SBX_CLOCK_MONOTONIC, 0};
  int32_t s = sbx_syscall(SBX_CALL_CLOCK_GETTIME, &r, sizeof r);
  if (!s) *ns = r.ns;
  return s;
}
/* Wait, without running, until the monotonic clock reaches deadline_ns (ABI 1.1).
   The heartbeat deadline keeps running: wake in time to report. */
static inline int32_t sbx_sleep_until(uint64_t deadline_ns) {
  sbx_sleep_rec r = {SBX_HEADER(sbx_sleep_rec), deadline_ns};
  return sbx_syscall(SBX_CALL_SLEEP_UNTIL, &r, sizeof r);
}
/* ABI 1.8: ring level and counters of an open /dev/audio. */
static inline int32_t sbx_audio_query(uint64_t audio_fd, sbx_audio_info *info) {
  *info = (sbx_audio_info){SBX_HEADER(sbx_audio_info), 0, 0, 0, 0, 0, 0, 0, 0};
  return sbx_ioctl(audio_fd, SBX_IOC_AUDIO_QUERY, info, sizeof *info);
}
static inline int32_t sbx_input_query(uint64_t input_fd, sbx_input_info *info) {
  *info = (sbx_input_info){SBX_HEADER(sbx_input_info), 0, 0, 0, 0, 0, 0, 0};
  return sbx_ioctl(input_fd, SBX_IOC_INPUT_QUERY, info, sizeof *info);
}
static inline int32_t sbx_input_state_get(uint64_t input_fd, sbx_input_state *state) {
  *state = (sbx_input_state){SBX_HEADER(sbx_input_state), {0}, {0}};
  return sbx_ioctl(input_fd, SBX_IOC_INPUT_STATE, state, sizeof *state);
}
/* /dev/serial0 (ABI 1.10): open it with SBX_O_NONBLOCK (read, write or both), then sbx_read and
   sbx_write as usual (read returns SBX_EAGAIN when empty; write always takes every byte). */
static inline int32_t sbx_serial_query(uint64_t serial_fd, sbx_serial_info *info) {
  *info = (sbx_serial_info){SBX_HEADER(sbx_serial_info), 0, 0, 0, 0, 0, 0, 0};
  return sbx_ioctl(serial_fd, SBX_IOC_SERIAL_QUERY, info, sizeof *info);
}
__attribute__((noreturn)) static inline void sbx_exit(int64_t status) {
  sbx_exit_rec r = {SBX_HEADER(sbx_exit_rec), status};
  sbx_syscall(SBX_CALL_EXIT, &r, sizeof r);
  for (;;) {}
}
/* ABI 1.11. Successful acquire writes token; release once per acquired token. */
static inline int32_t sbx_power_acquire(uint32_t flags,uint64_t *token) {
  sbx_power_hold_rec r = {SBX_HEADER(sbx_power_hold_rec),1,flags,0};
  int32_t s=sbx_syscall(SBX_CALL_POWER_HOLD,&r,sizeof r);
  if(!s) *token=r.token;
  return s;
}
static inline int32_t sbx_power_release(uint64_t token) {
  sbx_power_hold_rec r = {SBX_HEADER(sbx_power_hold_rec),0,0,token};
  return sbx_syscall(SBX_CALL_POWER_HOLD,&r,sizeof r);
}
/* ABI 1.12. *asleep gets the SBX_POWER_NO_* bits of what the kernel has put to sleep now: NO_BLANK set
 * means the screen is blanked (stop your sound and synthesis), NO_SLOWDOWN set means the clock is slowed.
 * Any input clears them. Needs startup->minor >= 12; older kernels return EINVAL. */
static inline int32_t sbx_power_asleep(uint32_t *asleep) {
  sbx_power_hold_rec r = {SBX_HEADER(sbx_power_hold_rec),2,0,0};
  int32_t s=sbx_syscall(SBX_CALL_POWER_HOLD,&r,sizeof r);
  if(!s) *asleep=(uint32_t)r.flags;
  return s;
}
static inline int32_t sbx_power_timeouts(uint32_t slow_seconds,uint32_t blank_seconds) {
  sbx_power_timeouts_rec r = {SBX_HEADER(sbx_power_timeouts_rec),slow_seconds,blank_seconds};
  return sbx_syscall(SBX_CALL_POWER_TIMEOUTS,&r,sizeof r);
}
#undef SBX_HEADER
#endif
