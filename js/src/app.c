/* SPDX-License-Identifier: MIT; see app.h. */
#include "app.h"
#include <stdio.h>
#include <string.h>

static const char *const cap_names[] = {"log", "time", "memory", "app.lifecycle", "fs.read", "fs.read.shared", "fs.write.saves",
  "fs.write.cache", "display", "input", "audio", "eink", "serial", "power"};
#define NCAPS ((unsigned)(sizeof cap_names / sizeof cap_names[0]))
const char *app_cap_name(unsigned i) {return i < NCAPS ? cap_names[i] : 0;}
uint32_t app_cap_bit(const char *name) {
  for (unsigned i = 0; i < NCAPS; i++) if (!strcmp(name, cap_names[i])) return 1u << i;
  return 0;
}

/* ---- the manifest: strict JSON, the keys below, nothing else (x-* keys are skipped) ---- */
struct jp {const char *s, *e; char *err; size_t cap;};
/* A message with one %s, which is replaced by a (no printf: the strings are all literals here). */
static int bad(struct jp *p, const char *fmt, const char *a) {
  if (!p->cap) return -1;
  const char *at = strstr(fmt, "%s");
  snprintf(p->err, p->cap, "%.*s%s%s", (int)(at ? (size_t)(at - fmt) : strlen(fmt)), fmt, a ? a : "", at ? at + 2 : "");
  return -1;
}
static void ws(struct jp *p) {while (p->s < p->e && (*p->s == ' ' || *p->s == '\t' || *p->s == '\r' || *p->s == '\n')) p->s++;}
static int lit(struct jp *p, char c) {ws(p); if (p->s < p->e && *p->s == c) {p->s++; return 0;} return -1;}
/* A string of printable ASCII; only \" \\ and \/ escapes. out may be NULL (skip). */
static int str(struct jp *p, char *out, size_t cap) {
  if (lit(p, '"')) return bad(p, "expected a string%s", 0);
  size_t n = 0;
  for (;;) {
    if (p->s >= p->e) return bad(p, "unterminated string%s", 0);
    unsigned char c = (unsigned char)*p->s++;
    if (c == '"') break;
    if (c == '\\') {
      if (p->s >= p->e || (*p->s != '"' && *p->s != '\\' && *p->s != '/')) return bad(p, "unsupported escape in a string%s", 0);
      c = (unsigned char)*p->s++;
    }
    if (c < 0x20 || c > 0x7e) return bad(p, "a string has a control or non-ASCII character%s", 0);
    if (out) {if (n + 1 >= cap) return bad(p, "a string is too long%s", 0); out[n] = (char)c;}
    n++;
  }
  if (out) out[n] = 0;
  return 0;
}
/* A whole number 0..65535 (no sign, fraction or exponent). */
static int whole(struct jp *p, uint32_t *v) {
  ws(p);
  if (p->s >= p->e || *p->s < '0' || *p->s > '9') return bad(p, "expected a whole number%s", 0);
  if (*p->s == '0' && p->s + 1 < p->e && p->s[1] >= '0' && p->s[1] <= '9') return bad(p, "a number has a leading zero%s", 0);
  uint32_t n = 0;
  while (p->s < p->e && *p->s >= '0' && *p->s <= '9') {n = n * 10 + (uint32_t)(*p->s++ - '0'); if (n > 65535) return bad(p, "a number is too large%s", 0);}
  if (p->s < p->e && (*p->s == '.' || *p->s == 'e' || *p->s == 'E')) return bad(p, "expected a whole number%s", 0);
  *v = n;
  return 0;
}
static int skip(struct jp *p, int depth) {
  ws(p);
  if (depth > 6) return bad(p, "nested too deeply%s", 0);
  if (p->s >= p->e) return bad(p, "unexpected end%s", 0);
  char c = *p->s;
  if (c == '"') return str(p, 0, 0);
  if (c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']'; p->s++; ws(p);
    if (p->s < p->e && *p->s == close) {p->s++; return 0;}
    for (;;) {
      if (c == '{' && (str(p, 0, 0) || lit(p, ':'))) return bad(p, "bad object%s", 0);
      if (skip(p, depth + 1)) return -1;
      ws(p);
      if (p->s < p->e && *p->s == ',') {p->s++; continue;}
      if (p->s < p->e && *p->s == close) {p->s++; return 0;}
      return bad(p, "expected , or a closing bracket%s", 0);
    }
  }
  if (p->e - p->s >= 4 && !strncmp(p->s, "true", 4)) {p->s += 4; return 0;}
  if (p->e - p->s >= 5 && !strncmp(p->s, "false", 5)) {p->s += 5; return 0;}
  if (p->e - p->s >= 4 && !strncmp(p->s, "null", 4)) {p->s += 4; return 0;}
  if (c == '-' || (c >= '0' && c <= '9')) {
    if (c == '-') p->s++;
    while (p->s < p->e && ((*p->s >= '0' && *p->s <= '9') || *p->s == '.' || *p->s == 'e' || *p->s == 'E' || *p->s == '+' || *p->s == '-')) p->s++;
    return 0;
  }
  return bad(p, "not JSON%s", 0);
}
static int is_name_char(char c) {return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';}
static int valid_version(const char *v) { /* 1 to 3 dot-separated numbers of 1 to 4 digits */
  int parts = 0;
  for (const char *p = v; ; ) {
    int d = 0;
    while (*p >= '0' && *p <= '9') {p++; d++;}
    if (d < 1 || d > 4) return 0;
    parts++;
    if (!*p) return parts <= 3;
    if (*p++ != '.') return 0;
  }
}
/* "MAIN.JS": 1 to 8 name characters and .JS, in either case; stored upper case. */
static int valid_entry(char *e) {
  size_t n = strlen(e);
  if (n < 4 || n > 11 || (e[n-3]!='.' || (e[n-2]|32)!='j' || (e[n-1]|32)!='s')) return 0;
  for (size_t i = 0; i < n - 3; i++) if (!is_name_char(e[i])) return 0;
  for (size_t i = 0; i < n; i++) if (e[i] >= 'a' && e[i] <= 'z') e[i] = (char)(e[i] - 32);
  return 1;
}
enum {K_SCHEMA = 1, K_NAME = 2, K_VERSION = 4, K_ENTRY = 8, K_API = 16, K_CAPS = 32, K_DESC = 64};
int app_manifest_parse(const char *json, uint32_t n, struct app_manifest *m, char *err, size_t errcap) {
  struct jp P = {json, json + n, err, errcap}, *p = &P;
  memset(m, 0, sizeof *m);
  if (err && errcap) err[0] = 0;
  if (n > APP_MANIFEST_MAX) return bad(p, "APP.JSON is over 2048 bytes%s", 0);
  if (lit(p, '{')) return bad(p, "APP.JSON must be a JSON object%s", 0);
  unsigned seen = 0; char key[24], text[8];
  ws(p);
  if (p->s < p->e && *p->s == '}') p->s++;
  else for (;;) {
    ws(p);
    if (str(p, key, sizeof key)) return bad(p, "a key is missing or too long%s", 0);
    if (lit(p, ':')) return bad(p, "expected : after \"%s\"", key);
    unsigned bit = !strcmp(key, "schema") ? K_SCHEMA : !strcmp(key, "name") ? K_NAME : !strcmp(key, "version") ? K_VERSION : !strcmp(key, "entry") ? K_ENTRY :
                   !strcmp(key, "api") ? K_API : !strcmp(key, "capabilities") ? K_CAPS : !strcmp(key, "description") ? K_DESC : 0;
    if (!bit) {
      if (strncmp(key, "x-", 2)) return bad(p, "unknown key \"%s\"", key);
      if (skip(p, 0)) return -1;
    } else {
      if (seen & bit) return bad(p, "key \"%s\" appears twice", key);
      seen |= bit;
      uint32_t v;
      switch (bit) {
      case K_SCHEMA: if (whole(p, &v)) return bad(p, "\"schema\" must be a whole number%s", 0); if (v != 1) return bad(p, "\"schema\" must be 1%s", 0); break;
      case K_NAME: if (str(p, m->name, sizeof m->name)) return bad(p, "\"name\" must be text of 1 to 32 characters%s", 0); break;
      case K_VERSION: if (str(p, m->version, sizeof m->version)) return bad(p, "\"version\" must be text such as 1.0.2%s", 0); break;
      case K_ENTRY: if (str(p, m->entry, sizeof m->entry)) return bad(p, "\"entry\" must be a file name such as MAIN.JS%s", 0); break;
      case K_API: if (whole(p, &v)) return bad(p, "\"api\" must be a whole number%s", 0); if (v < 1) return bad(p, "\"api\" must be 1 or more%s", 0); m->api = v; break;
      case K_DESC: {char d[121]; if (str(p, d, sizeof d)) return bad(p, "\"description\" is too long (120 characters at most)%s", 0); break;}
      case K_CAPS: {
        if (lit(p, '[')) return bad(p, "\"capabilities\" must be a list%s", 0);
        ws(p);
        if (p->s < p->e && *p->s == ']') {p->s++; break;}
        for (int count = 0;; count++) {
          char c[24];
          if (count >= 16) return bad(p, "too many capabilities%s", 0);
          if (str(p, c, sizeof c)) return bad(p, "a capability name is bad or too long%s", 0);
          uint32_t b = app_cap_bit(c);
          if (!b) return bad(p, "unknown capability \"%s\"", c);
          if (m->caps & b) return bad(p, "capability \"%s\" is listed twice", c);
          m->caps |= b;
          ws(p);
          if (p->s < p->e && *p->s == ',') {p->s++; continue;}
          if (lit(p, ']')) return bad(p, "expected , or ] in \"capabilities\"%s", 0);
          break;
        }
        break;
      }
      }
    }
    ws(p);
    if (p->s < p->e && *p->s == ',') {p->s++; continue;}
    if (lit(p, '}')) return bad(p, "expected , or } (is a quote or comma missing?)%s", 0);
    break;
  }
  ws(p);
  if (p->s != p->e) return bad(p, "text after the closing }%s", 0);
  (void)text;
  if (!(seen & K_NAME) || !m->name[0]) return bad(p, "\"name\" is required%s", 0);
  if (!(seen & K_API)) return bad(p, "\"api\" is required (the runtime API your app needs)%s", 0);
  if (seen & K_VERSION && !valid_version(m->version)) return bad(p, "\"version\" must look like 1.0.2%s", 0);
  if (!(seen & K_VERSION)) strcpy(m->version, "0");
  if (seen & K_ENTRY) {if (!valid_entry(m->entry)) return bad(p, "\"entry\" must be NAME.JS (1 to 8 letters, digits, _ or -)%s", 0);}
  else strcpy(m->entry, "MAIN.JS");
  m->caps |= APP_CAP_BASE;
  return 0;
}

/* ---- paths ---- */
static int ieq(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return 1;
}
/* path is at or under folder (a component boundary, either case, the card has no case). */
static int under(const char *path, const char *folder) {
  size_t n = strlen(folder);
  return strlen(path) >= n && ieq(path, folder, n) && (!path[n] || path[n] == '/');
}
void app_policy_init(struct app_policy *p, const char *id, uint32_t caps) {
  snprintf(p->dir, sizeof p->dir, "/sd/OS/APPS/%s", id);
  snprintf(p->saves, sizeof p->saves, "/sd/OS/SAVES/%s", id);
  snprintf(p->cache, sizeof p->cache, "/sd/OS/CACHE/%s", id);
  p->caps = caps;
}
int app_path_check(const struct app_policy *p, const char *path, int write, const char **why) {
  static const char *none; if (!why) why = &none;
  size_t len = strlen(path);
  if (len > 95) {*why = "the path is too long"; return -1;}
  if (strncmp(path, "/sd/", 4)) {*why = !strncmp(path, "/dev", 4) ? "device paths are not allowed" : "paths start with /sd/"; return -1;}
  for (const char *c = path + 4; ; ) { /* each component: no . or .., no FAT-ignored trailing dot or space, no odd characters */
    const char *e = c; while (*e && *e != '/') e++;
    size_t n = (size_t)(e - c);
    if (!n) {*why = "an empty folder name (//) is not allowed"; return -1;}
    if ((n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) {*why = "'.' and '..' are not allowed"; return -1;}
    if (c[n - 1] == '.' || c[n - 1] == ' ') {*why = "a name ends in a dot or space"; return -1;}
    for (size_t i = 0; i < n; i++) {
      unsigned char ch = (unsigned char)c[i];
      if (ch < 0x20 || ch > 0x7e || strchr("\\:*?\"<>|~", ch)) {*why = "a name has a character the card does not allow"; return -1;}
    }
    if (!*e) break;
    c = e + 1;
    if (!*c) {*why = "an empty folder name (//) is not allowed"; return -1;}
  }
  if (write) {
    if (under(path, p->saves) && strlen(path) > strlen(p->saves)) {
      if (p->caps & APP_CAP_FS_WRITE_SAVES) return 0;
      *why = "the app has no fs.write.saves capability"; return -1;
    }
    if (strlen(path) == strlen(p->cache) && ieq(path, p->cache, strlen(path))) {
      if (p->caps & APP_CAP_FS_WRITE_CACHE) return 0;
      *why = "the app has no fs.write.cache capability"; return -1;
    }
    *why = under(path, p->dir) ? "the app's own folder is read-only" : "writes go to /sd/OS/SAVES/<id>/ only";
    return -1;
  }
  if (under(path, p->saves) && p->caps & (APP_CAP_FS_READ | APP_CAP_FS_WRITE_SAVES)) return 0;
  if (under(path, p->dir) && p->caps & APP_CAP_FS_READ) return 0;
  if (under(path, "/sd/OS")) {
    *why = under(path, "/sd/OS/APPS") && !under(path, p->dir) ? "another app's folder is not readable" : "outside the app's own folders";
    return -1;
  }
  if (p->caps & APP_CAP_FS_READ_SHARED) return 0;
  *why = "outside the app's own folders (fs.read.shared would allow reading the card outside /sd/OS)";
  return -1;
}

const char *app_id_of(const char *dir) {
  static const char pre[] = "/sd/OS/APPS/";
  if (strncmp(dir, pre, sizeof pre - 1)) return 0;
  const char *id = dir + sizeof pre - 1; size_t n = strlen(id);
  if (n < 1 || n > 8) return 0;
  for (size_t i = 0; i < n; i++) if (!is_name_char(id[i])) return 0;
  return id;
}

/* ---- AUTORUN.TXT ---- */
int app_autorun_parse(const char *text, uint32_t n, char *dir, size_t dircap, char *err, size_t errcap) {
  char app[64] = "APPS/HELLO"; /* the default when line 2 is absent */
  dir[0] = 0;
  uint32_t i = 0;
  while (i < n && text[i] != '\n') i++;
  if (i < n) { /* line 2 */
    i++;
    while (i < n && text[i] == ' ') i++;
    size_t k = 0;
    while (i + k < n && text[i + k] != '\r' && text[i + k] != '\n' && text[i + k] != ' ' && text[i + k] != 0 && k < sizeof app - 1) {app[k] = text[i + k]; k++;}
    if (k) app[k] = 0;
  }
  for (char *c = app; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
  if (!strcmp(app, "PICKER")) return 0;
  if (!strncmp(app, "APPS/", 5) && app_id_of((snprintf(dir, dircap, "/sd/OS/%s", app), dir))) return 0;
  dir[0] = 0;
  if (errcap) snprintf(err, errcap, "AUTORUN names \"%.24s\": use APPS/NAME or PICKER", app);
  return -1;
}
