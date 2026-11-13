/* SPDX-License-Identifier: MIT; what newlib (the toolchain's libc and libm)
 * needs under JS.ELF: one fixed heap for malloc (QuickJS-ng and the element
 * tree share it), writes to the log, and nothing else (no files through stdio,
 * no processes, no threads). */
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "plat.h"
#define HEAP_BYTES (3u << 20)
static uint8_t heap[HEAP_BYTES] __attribute__((aligned(16)));
static uint32_t brk_at;
uint32_t sys_heap_used(void);
uint32_t sys_heap_used(void) {return brk_at;}
void *_sbrk(intptr_t n);
void *_sbrk(intptr_t n) {
  if (n < 0 ? (uint32_t)-n > brk_at : (uint32_t)n > HEAP_BYTES - brk_at) {errno = ENOMEM; return (void *)-1;}
  void *p = heap + brk_at; brk_at += (uint32_t)n; return p;
}
int _write(int fd, const char *b, int n);
int _write(int fd, const char *b, int n) {(void)fd; plat_log(b, (uint32_t)n); return n;}
int _read(int fd, char *b, int n);
int _read(int fd, char *b, int n) {(void)fd; (void)b; (void)n; return 0;}
int _close(int fd);
int _close(int fd) {(void)fd; return -1;}
int _fstat(int fd, struct stat *s);
int _fstat(int fd, struct stat *s) {(void)fd; s->st_mode = S_IFCHR; return 0;}
int _isatty(int fd);
int _isatty(int fd) {(void)fd; return 1;}
int _lseek(int fd, int o, int w);
int _lseek(int fd, int o, int w) {(void)fd; (void)o; (void)w; return 0;}
void _exit(int code);
void _exit(int code) {(void)code; for (;;) plat_sleep_until(~0ull);}
int _kill(int pid, int sig);
int _kill(int pid, int sig) {(void)pid; (void)sig; errno = EINVAL; return -1;}
int _getpid(void);
int _getpid(void) {return 1;}
int _gettimeofday(struct timeval *t, void *z);
int _gettimeofday(struct timeval *t, void *z) {
  (void)z; uint64_t ns = plat_clock_ns(); /* no wall clock: time since boot */
  t->tv_sec = (time_t)(ns / 1000000000ull); t->tv_usec = (suseconds_t)(ns / 1000ull % 1000000ull); return 0;
}
