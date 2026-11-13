/* SPDX-License-Identifier: MIT; a command-line view of src/app.c for the tests (tests/app_run.py compares it with
 * tools/app_check.py). Not part of JS.ELF.
 *   appcheck manifest <file>                      -> "ok name=.. version=.. entry=.. api=N caps=a,b" or "err <message>"
 *   appcheck path <id> <caps,..|all|none> r|w <path>  -> "allow" or "deny <reason>"
 *   appcheck autorun <file>                       -> "ok /sd/OS/APPS/X" | "ok picker" | "err <message>"
 *   appcheck batch                                -> stdin, one case a line, hex for the bytes (tests/fuzz_run.py):
 *        M <hex>                      a manifest             -> as "manifest"
 *        A <hex>                      an AUTORUN.TXT         -> as "autorun"
 *        P <id> <caps|all|none> r|w <hex>   a path           -> as "path" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
static int unhex(const char *h, char *out, size_t cap) { /* hex to bytes; the byte count, or -1 */
  size_t n = strlen(h);
  if (n % 2 || n / 2 >= cap) return -1;
  for (size_t i = 0; i < n; i += 2) {unsigned v; if (sscanf(h + i, "%2x", &v) != 1) return -1; out[i / 2] = (char)v;}
  out[n / 2] = 0;
  return (int)(n / 2);
}
static void manifest_line(const char *buf, uint32_t n) {
  char err[120] = ""; struct app_manifest m;
  if (app_manifest_parse(buf, n, &m, err, sizeof err)) {printf("err %s\n", err); return;}
  printf("ok name=%s version=%s entry=%s api=%u caps=", m.name, m.version, m.entry, (unsigned)m.api);
  int first = 1;
  for (unsigned i = 4; app_cap_name(i); i++) if (m.caps & (1u << i)) {printf("%s%s", first ? "" : ",", app_cap_name(i)); first = 0;}
  putchar('\n');
}
static void path_line(const char *id, char *caps_s, const char *mode, const char *path) {
  uint32_t caps = APP_CAP_BASE;
  if (!strcmp(caps_s, "all")) caps = APP_CAP_ALL;
  else if (strcmp(caps_s, "none")) for (char *t = strtok(caps_s, ","); t; t = strtok(0, ",")) caps |= app_cap_bit(t);
  struct app_policy p; const char *why = "";
  app_policy_init(&p, id, caps);
  if (app_path_check(&p, path, mode[0] == 'w', &why)) printf("deny %s\n", why); else puts("allow");
}
static int batch(void) {
  static char line[20000], bytes[10000];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\n")] = 0;
    char *sp[5] = {0}; int k = 0;
    for (char *t = strtok(line, " "); t && k < 5; t = strtok(0, " ")) sp[k++] = t;
    if (!k) {puts("skip"); continue;}
    if (sp[0][0] == 'M' && k == 2) {int n = unhex(sp[1], bytes, sizeof bytes); if (n < 0) puts("skip"); else manifest_line(bytes, (uint32_t)n);}
    else if (sp[0][0] == 'M' && k == 1) manifest_line("", 0);
    else if (sp[0][0] == 'A') {
      int n = k == 2 ? unhex(sp[1], bytes, sizeof bytes) : (bytes[0] = 0);
      char err[120] = "", dir[48];
      if (n < 0) {puts("skip"); continue;}
      if (app_autorun_parse(bytes, (uint32_t)n, dir, sizeof dir, err, sizeof err)) printf("err %s\n", err); else printf("ok %s\n", dir[0] ? dir : "picker");
    } else if (sp[0][0] == 'P' && k >= 4) {int n = k == 5 ? unhex(sp[4], bytes, sizeof bytes) : (bytes[0] = 0); if (n < 0) puts("skip"); else path_line(sp[1], sp[2], sp[3], bytes);}
    else puts("skip");
    fflush(stdout);
  }
  return 0;
}
int main(int argc, char **argv) {
  static char buf[8192];
  if (argc == 2 && !strcmp(argv[1], "batch")) return batch();
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
