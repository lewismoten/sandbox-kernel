/* SPDX-License-Identifier: MIT
 * sandbox-kernel SDK: raw ARM32 system-call wrappers (ARM state, soft-float).
 * svc #0 with r7 = call, r0 = record, r1 = record bytes, r2 = ABI major,
 * r3-r6 = 0. Each wrapper returns 0 or a negative SBX_E* value; a libc layer
 * can turn that into -1 and errno later. */
#ifndef SANDBOX_SYSCALL_H
#define SANDBOX_SYSCALL_H
#include "abi.h"
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

#define SBX_HEADER(type) {sizeof(type), 0, 0, 0}

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
static inline int32_t sbx_input_query(uint64_t input_fd, sbx_input_info *info) {
  *info = (sbx_input_info){SBX_HEADER(sbx_input_info), 0, 0, 0, 0, 0, 0, 0};
  return sbx_ioctl(input_fd, SBX_IOC_INPUT_QUERY, info, sizeof *info);
}
static inline int32_t sbx_input_state_get(uint64_t input_fd, sbx_input_state *state) {
  *state = (sbx_input_state){SBX_HEADER(sbx_input_state), {0}, {0}};
  return sbx_ioctl(input_fd, SBX_IOC_INPUT_STATE, state, sizeof *state);
}
__attribute__((noreturn)) static inline void sbx_exit(int64_t status) {
  sbx_exit_rec r = {SBX_HEADER(sbx_exit_rec), status};
  sbx_syscall(SBX_CALL_EXIT, &r, sizeof r);
  for (;;) {}
}
#undef SBX_HEADER
#endif
