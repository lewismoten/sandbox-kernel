/* SPDX-License-Identifier: MIT; JS.ELF: starts the app named on the second
 * line of /OS/AUTORUN.TXT (a folder under /OS, such as APPS/HELLO; that is
 * the default), runs its MAIN.JS and keeps the loop turning. The first line
 * is the kernel's (JS.ELF). App exit returns to the built-in picker;
 * PICKER on line 2 starts there directly. Quitting the picker returns to
 * the kernel through crt0's existing entry-return path. */
#include <sandbox/syscall.h>
#include <stdio.h>
#include <string.h>
#include "jsrt.h"
#include "plat.h"
int plat_sbx_init(const sbx_startup *start);
uint32_t sys_heap_used(void);
static char text[512];
int main(const sbx_startup *start) {
  int r = plat_sbx_init(start);
  if (r) return r;
  char app[64] = "APPS/HELLO", line[120];
  int32_t n = plat_read("/sd/OS/AUTORUN.TXT", (uint8_t *)text, sizeof text - 1);
  if (n > 0) {
    text[n] = 0;
    char *p = strchr(text, '\n');
    if (p) {
      p++;
      size_t k = 0;
      while (*p == ' ') p++;
      while (p[k] && p[k] != '\r' && p[k] != '\n' && p[k] != ' ' && k < sizeof app - 1) k++;
      if (k) {memcpy(app, p, k); app[k] = 0;}
    }
  }
  snprintf(line, sizeof line, "JS.ELF QuickJS-ng %s, ABI %u.%u, app /OS/%s", "0.18.0", (unsigned)start->major, (unsigned)start->minor, app);
  plat_log(line, (uint32_t)strlen(line));
  char dir[80]; snprintf(dir, sizeof dir, "/sd/OS/%s", app);
  if (jsapp_start(!strcmp(app, "PICKER") ? 0 : dir, 3u << 20)) return 5;
  snprintf(line, sizeof line, "JS heap high-water after start %u bytes", (unsigned)sys_heap_used());
  plat_log(line, (uint32_t)strlen(line));
  for (;;) {
    uint64_t wake = jsapp_step();
    if (!wake) break;
    plat_sleep_until(wake);
  }
  return jsrt_exit_code();
}
