/* SPDX-License-Identifier: MIT; a command-line view of src/app.c for the tests (tests/app_run.py compares it with
 * tools/app_check.py). Not part of JS.ELF.
 *   appcheck manifest <file>                      -> "ok name=.. version=.. entry=.. api=N caps=a,b" or "err <message>"
 *   appcheck path <id> <caps,..|all|none> r|w <path>  -> "allow" or "deny <reason>"
 *   appcheck autorun <file>                       -> "ok /sd/OS/APPS/X" | "ok picker" | "err <message>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
int main(int argc, char **argv) {
  static char buf[8192];
  if (argc >= 3 && (!strcmp(argv[1], "manifest") || !strcmp(argv[1], "autorun"))) {
    FILE *f = fopen(argv[2], "rb");
    if (!f) {puts("err cannot read"); return 2;}
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    char err[120] = "";
    if (!strcmp(argv[1], "autorun")) {
      char dir[48];
      if (app_autorun_parse(buf, (uint32_t)n, dir, sizeof dir, err, sizeof err)) printf("err %s\n", err);
      else printf("ok %s\n", dir[0] ? dir : "picker");
      return 0;
    }
    struct app_manifest m;
    if (app_manifest_parse(buf, (uint32_t)n, &m, err, sizeof err)) {printf("err %s\n", err); return 0;}
    printf("ok name=%s version=%s entry=%s api=%u caps=", m.name, m.version, m.entry, (unsigned)m.api);
    int first = 1;
    for (unsigned i = 4; app_cap_name(i); i++) if (m.caps & (1u << i)) {printf("%s%s", first ? "" : ",", app_cap_name(i)); first = 0;}
    putchar('\n');
    return 0;
  }
  if (argc == 6 && !strcmp(argv[1], "path")) {
    uint32_t caps = APP_CAP_BASE;
    if (!strcmp(argv[3], "all")) caps = APP_CAP_ALL;
    else if (strcmp(argv[3], "none"))
      for (char *t = strtok(argv[3], ","); t; t = strtok(0, ",")) caps |= app_cap_bit(t);
    struct app_policy p; const char *why = "";
    app_policy_init(&p, argv[2], caps);
    if (app_path_check(&p, argv[5], argv[4][0] == 'w', &why)) printf("deny %s\n", why);
    else puts("allow");
    return 0;
  }
  fprintf(stderr, "usage: appcheck manifest|autorun <file> | path <id> <caps> r|w <path>\n");
  return 2;
}
