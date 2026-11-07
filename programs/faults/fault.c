/* SPDX-License-Identifier: MIT; fault programs for acceptance tests 2 and 5,
 * built only against sdk/. Each logs what it is about to do, then does it;
 * the kernel must catch it, log it, and keep running. FAULT selects one. */
#include <sandbox/syscall.h>
static void say(uint64_t fd, const char *s) {unsigned n = 0; while (s[n]) n++; sbx_log(fd, s, n);}
#if FAULT == 6
static uint32_t data_word = 0xe12fff1eu; /* "bx lr", in a never-executable page */
#endif
int main(void) {
  uint64_t log;
  if (sbx_open("/dev/log", 8, SBX_O_WRONLY, &log)) return 3;
#if FAULT == 1
  say(log, "FAULT privileged instruction (write SCTLR)");
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(0));
#elif FAULT == 2
  say(log, "FAULT write to text");
  *(volatile uint32_t *)(uintptr_t)&main = 0;
#elif FAULT == 3
  say(log, "FAULT infinite loop");
  for (;;) __asm__ volatile("");
#elif FAULT == 4
  say(log, "FAULT read kernel memory");
  (void)*(volatile uint32_t *)0x00100000u;
#elif FAULT == 5
  say(log, "FAULT write MMIO (GPIO)");
  *(volatile uint32_t *)0x20200000u = 0;
#elif FAULT == 6
  say(log, "FAULT execute writable data");
  ((void (*)(void))(uintptr_t)&data_word)();
#elif FAULT == 7
  say(log, "FAULT stack overflow into the guard");
  volatile uint8_t *sp = (volatile uint8_t *)__builtin_frame_address(0);
  for (;;) {sp -= 4096; *sp = 1;}
#endif
  say(log, "FAULT was not caught");
  return 9;
}
