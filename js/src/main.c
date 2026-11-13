/* SPDX-License-Identifier: MIT; JS.ELF: starts the app named on the second
 * line of /OS/AUTORUN.TXT (APPS/NAME, or PICKER; the default is APPS/HELLO), runs its MAIN.JS and keeps the loop turning. The first line
 * is the kernel's (JS.ELF). App exit returns to the built-in picker;
 * PICKER on line 2 starts there directly. Quitting the picker returns to
 * the kernel through crt0's existing entry-return path. */
#include <sandbox/syscall.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "jsrt.h"
#include "plat.h"
int plat_sbx_init(const sbx_startup *start);
uint32_t sys_heap_used(void);
int main(const sbx_startup *start) {
  int r = plat_sbx_init(start);
  if (r) return r;
  char line[120];
  snprintf(line, sizeof line, "JS.ELF QuickJS-ng %s, ABI %u.%u, runtime %s API %u", "0.18.0", (unsigned)start->major, (unsigned)start->minor, JSRT_VERSION, APP_API_VERSION);
  plat_log(line, (uint32_t)strlen(line));
  if (jsapp_autorun(JSRT_HEAP_LIMIT)) return 5;
  snprintf(line, sizeof line, "JS heap high-water after start %u bytes", (unsigned)sys_heap_used());
  plat_log(line, (uint32_t)strlen(line));
  for (;;) {
    uint64_t wake = jsapp_step();
    if (!wake) break;
    plat_sleep_until(wake);
  }
  return jsrt_exit_code();
}
