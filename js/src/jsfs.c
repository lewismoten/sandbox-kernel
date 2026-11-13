/* SPDX-License-Identifier: MIT; the app's file API (slice 4a), see jsfs.h and docs/apps.md.
 * Reads are real kernel handles (open, read, seek, close). Writes are staged in the app's memory and published by the
 * kernel's only write, a whole-file WRONLY|CREAT|TRUNC to a save or cache name, at commit; a writer that is dropped, aborted
 * or left open at stop never touches the card. Every file or writer is one JSRT_H_FILE entry in the handle table. */
#include "jsfs.h"
#include "app.h"
#include "handle.h"
#include "plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANY_READ (APP_CAP_FS_READ | APP_CAP_FS_READ_SHARED | APP_CAP_FS_WRITE_SAVES)
#define ANY_WRITE (APP_CAP_FS_WRITE_SAVES | APP_CAP_FS_WRITE_CACHE)
uint32_t jsrt_write_quota = 256u * 1024u;
/* A file handle (reader) or a staged writer; kind tells which. The handle table owns it. */
struct fobj {int writer; uint64_t fd; uint32_t size, pos; char path[96]; uint8_t *buf; uint32_t n, cap;};
static JSClassID file_class, writer_class;
static int writer_open;

static void fobj_release(void *u) {
  struct fobj *f = u;
  if (f->writer) {writer_open = 0; free(f->buf);} else plat_fclose(f->fd);
  free(f);
}
static void fobj_finalizer(JSRuntime *r, JSValueConst v) {
  (void)r; JSClassID k = JS_GetClassID(v);
  uintptr_t id = (uintptr_t)JS_GetOpaque(v, k);
  if (id) jsrt_handle_close((uint32_t)id);
}
static const char *errno_name(int e, const char **why) {
  switch (e) {
  case 2: *why = "no such file or folder"; return "ENOENT";
  case 13: *why = "not allowed"; return "EACCES";
  case 16: *why = "another file or writer is open on the save area; close read handles there and try again"; return "EBUSY";
  case 22: *why = "the kernel refused the request"; return "EINVAL";
  case 24: *why = "too many open files"; return "EMFILE";
  case 28: *why = "the card is full, or the file is over the kernel's 1 MiB single-write limit"; return "ENOSPC";
  case 30: *why = "this path is read-only for apps"; return "EROFS";
  case 36: *why = "the name is too long"; return "ENAMETOOLONG";
  case 122: *why = "the kernel allows 64 game folders and 32 files per game"; return "EDQUOT";
  default: *why = "the card reported an error"; return "EIO";
  }
}
/* Throws Error("CODE: text") with .code = "CODE". Returns JS_EXCEPTION. */
static JSValue fail(JSContext *c, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static JSValue fail(JSContext *c, const char *code, const char *fmt, ...) {
  char text[200]; va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof text, fmt, ap); va_end(ap);
  char msg[240]; snprintf(msg, sizeof msg, "%s: %s", code, text);
  JSValue e = JS_NewError(c);
  JS_SetPropertyStr(c, e, "message", JS_NewString(c, msg));
  JS_SetPropertyStr(c, e, "code", JS_NewString(c, code));
  return JS_Throw(c, e);
}
static JSValue fail_errno(JSContext *c, int err, const char *what, const char *path) {
  const char *why; const char *code = errno_name(err < 0 ? -err : err, &why);
  return fail(c, code, "%s %s: %s", what, path, why);
}

/* ---- stat and listing, from the parent folder's entries (the kernel has no stat call) ---- */
struct entry {char name[256]; int dir; uint32_t size, modified, created;};
struct find {const char *want; size_t n; int found; struct entry e;};
static int ieq_n(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return 1;
}
static void find_cb(void *user, const char *name, uint32_t n, int dir, uint32_t size, uint32_t modified, uint32_t created) {
  struct find *q = user;
  if (q->found || n != q->n || n >= sizeof q->e.name || !ieq_n(name, q->want, n)) return;
  memcpy(q->e.name, name, n); q->e.name[n] = 0; q->e.dir = dir; q->e.size = size; q->e.modified = modified; q->e.created = created; q->found = 1;
}
/* 0 and *out, or a negative errno. The name is looked up as a long name, then as an 8.3 alias. */
static int stat_path(const char *path, struct entry *out) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash == path) return -2;
  size_t plen = (size_t)(slash - path);
  if (plen <= 3) { /* "/sd/X": the root folder's own entries */
    char root[4] = "/sd"; if (!slash[1]) return -22;
    struct find q = {slash + 1, strlen(slash + 1), 0, {{0}, 0, 0, 0, 0}};
    int32_t r = plat_list_meta(root, 0, find_cb, &q);
    if (r < 0) return r;
    if (!q.found) {q.found = 0; plat_list_meta(root, 1, find_cb, &q);}
    if (!q.found) return -2;
    *out = q.e; return 0;
  }
  char parent[96]; if (plen >= sizeof parent) return -36;
  memcpy(parent, path, plen); parent[plen] = 0;
  struct find q = {slash + 1, strlen(slash + 1), 0, {{0}, 0, 0, 0, 0}};
  if (!q.n) return -22;
  int32_t r = plat_list_meta(parent, 0, find_cb, &q);
  if (r < 0) return r;
  if (!q.found) {plat_list_meta(parent, 1, find_cb, &q);}
  if (!q.found) return -2;
  *out = q.e; return 0;
}
struct lister {JSContext *c; JSValue a; uint32_t n;};
static JSValue entry_obj(JSContext *c, const char *name, uint32_t n, int dir, uint32_t size, uint32_t modified, uint32_t created) {
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "name", JS_NewStringLen(c, name, n));
  JS_SetPropertyStr(c, o, "type", JS_NewString(c, dir ? "dir" : "file"));
  JS_SetPropertyStr(c, o, "size", JS_NewUint32(c, dir ? 0 : size));
  JS_SetPropertyStr(c, o, "modified", JS_NewUint32(c, modified));
  JS_SetPropertyStr(c, o, "created", JS_NewUint32(c, created));
  return o;
}
static void list_cb(void *user, const char *name, uint32_t n, int dir, uint32_t size, uint32_t modified, uint32_t created) {
  struct lister *l = user;
  if (name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.'))) return;
  JS_SetPropertyUint32(l->c, l->a, l->n++, entry_obj(l->c, name, n, dir, size, modified, created));
}
static JSValue js_readdir_meta(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char path[96];
  if (jsrt_need(c, ANY_READ, "fs.readdir") || jsrt_copy_str(c, a[0], path, sizeof path)) return JS_EXCEPTION;
  const char *why;
  if (jsrt_path_check(path, 0, &why)) return fail(c, "EACCES", "fs.readdir: path refused, %s: %s", why, path);
  struct lister l = {c, JS_NewArray(c), 0};
  int32_t r = plat_list_meta(path, 0, list_cb, &l);
  if (r < 0) {JS_FreeValue(c, l.a); return fail_errno(c, r, "fs.readdir", path);}
  return l.a;
}
static JSValue js_stat(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char path[96]; struct entry e;
  if (jsrt_need(c, ANY_READ, "fs.stat") || jsrt_copy_str(c, a[0], path, sizeof path)) return JS_EXCEPTION;
  const char *why;
  if (jsrt_path_check(path, 0, &why)) return fail(c, "EACCES", "fs.stat: path refused, %s: %s", why, path);
  int r = stat_path(path, &e);
  if (r) return fail_errno(c, r, "fs.stat", path);
  return entry_obj(c, e.name, (uint32_t)strlen(e.name), e.dir, e.size, e.modified, e.created);
}

/* ---- file handles ---- */
static struct fobj *get(JSContext *c, JSValueConst v, JSClassID k) {
  uint32_t id = (uint32_t)(uintptr_t)JS_GetOpaque(v, k);
  struct fobj *f = id ? jsrt_handle_get(id, JSRT_H_FILE) : 0;
  if (!f) JS_ThrowPlainError(c, "EBADF: the file handle is closed or stale");
  return f;
}
static JSValue f_size(JSContext *c, JSValueConst t) {struct fobj *f = get(c, t, file_class); return f ? JS_NewUint32(c, f->size) : JS_EXCEPTION;}
static JSValue f_pos(JSContext *c, JSValueConst t) {struct fobj *f = get(c, t, file_class); return f ? JS_NewUint32(c, f->pos) : JS_EXCEPTION;}
static JSValue f_path(JSContext *c, JSValueConst t) {struct fobj *f = get(c, t, file_class); return f ? JS_NewString(c, f->path) : JS_EXCEPTION;}
static int len_arg(JSContext *c, int n, JSValueConst *a, int at, uint32_t rest, uint32_t cap, uint32_t *len) {
  *len = rest < cap ? rest : cap;
  if (n <= at || JS_IsUndefined(a[at])) return 0;
  double d; if (JS_ToFloat64(c, &d, a[at])) return -1;
  if (!(d >= 0) || d != (double)(uint32_t)d) {JS_ThrowRangeError(c, "length must be a whole number of bytes, 0 or more"); return -1;}
  if ((uint32_t)d < *len) *len = (uint32_t)d;
  return 0;
}
/* Reads up to len bytes into dst; returns bytes read, or -errno. */
static int32_t pull(struct fobj *f, uint8_t *dst, uint32_t len) {
  uint32_t have = 0;
  while (have < len) {
    uint32_t got; int r = plat_fread(f->fd, dst + have, len - have, &got);
    if (r) return r;
    if (!got) break;
    have += got;
  }
  f->pos += have;
  return (int32_t)have;
}
/* read(length?) -> Uint8Array of up to length bytes (default and most: 256 KiB; fewer at the end; empty at the end). */
static JSValue f_read(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  struct fobj *f = get(c, t, file_class); uint32_t len; size_t cap; uint8_t *io = jsrt_io(&cap);
  if (!f || len_arg(c, n, a, 0, f->size - f->pos, (uint32_t)cap, &len)) return JS_EXCEPTION;
  int32_t got = pull(f, io, len);
  if (got < 0) return fail_errno(c, got, "read", f->path);
  return JS_NewUint8ArrayCopy(c, io, (size_t)got);
}
/* The bytes of an ArrayBuffer or typed array. */
static int bytes_of(JSContext *c, JSValueConst v, uint8_t **p, size_t *len) {
  size_t off, bl, bpe;
  if (JS_IsArrayBuffer(v)) {*p = JS_GetArrayBuffer(c, len, v); return *p ? 0 : -1;}
  JSValue ab = JS_GetTypedArrayBuffer(c, v, &off, &bl, &bpe);
  if (JS_IsException(ab)) {JS_FreeValue(c, JS_GetException(c)); JS_ThrowTypeError(c, "expected a string, an ArrayBuffer or a typed array"); return -1;}
  size_t total; uint8_t *base = JS_GetArrayBuffer(c, &total, ab); JS_FreeValue(c, ab);
  if (!base) return -1;
  *p = base + off; *len = bl; return 0;
}
/* readInto(view, offset = 0, length = rest of view) -> bytes read (0 at the end). */
static JSValue f_read_into(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  struct fobj *f = get(c, t, file_class); uint8_t *p; size_t room;
  if (!f || n < 1 || bytes_of(c, a[0], &p, &room)) return JS_EXCEPTION;
  uint32_t off = 0, len;
  if (n > 1 && !JS_IsUndefined(a[1])) {double d; if (JS_ToFloat64(c, &d, a[1])) return JS_EXCEPTION; if (!(d >= 0) || d > (double)room || d != (double)(uint32_t)d) return JS_ThrowRangeError(c, "offset is outside the buffer"); off = (uint32_t)d;}
  if (len_arg(c, n, a, 2, f->size - f->pos, (uint32_t)(room - off), &len)) return JS_EXCEPTION;
  if (n > 2 && !JS_IsUndefined(a[2])) {double d; JS_ToFloat64(c, &d, a[2]); if (d > (double)(room - off)) return JS_ThrowRangeError(c, "length runs past the end of the buffer");}
  int32_t got = pull(f, p + off, len);
  if (got < 0) return fail_errno(c, got, "read", f->path);
  return JS_NewUint32(c, (uint32_t)got);
}
/* text(length?) -> the next bytes (at most 256 KiB) as a UTF-8 string. */
static JSValue f_text(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  struct fobj *f = get(c, t, file_class); uint32_t len; size_t cap; uint8_t *io = jsrt_io(&cap);
  if (!f || len_arg(c, n, a, 0, f->size - f->pos, (uint32_t)cap, &len)) return JS_EXCEPTION;
  int32_t got = pull(f, io, len);
  if (got < 0) return fail_errno(c, got, "read", f->path);
  return JS_NewStringLen(c, (const char *)io, (size_t)got);
}
/* seek(position) -> position; 0 to size inclusive (the end is allowed; the kernel seeks only from the start). */
static JSValue f_seek(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  struct fobj *f = get(c, t, file_class); double d;
  if (!f) return JS_EXCEPTION;
  if (n < 1 || JS_ToFloat64(c, &d, a[0])) return n < 1 ? JS_ThrowTypeError(c, "seek(position) needs a position") : JS_EXCEPTION;
  if (!(d >= 0) || d > (double)f->size || d != (double)(uint32_t)d) return JS_ThrowRangeError(c, "seek: position %.0f is outside 0 to %u", d, (unsigned)f->size);
  int r = plat_fseek(f->fd, (uint32_t)d);
  if (r) return fail_errno(c, r, "seek", f->path);
  f->pos = (uint32_t)d;
  return JS_NewUint32(c, f->pos);
}
static JSValue f_close(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)n; (void)a;
  uint32_t id = (uint32_t)(uintptr_t)JS_GetOpaque(t, file_class);
  if (!id || jsrt_handle_close(id)) return JS_ThrowPlainError(c, "EBADF: the file handle is closed or stale");
  return JS_UNDEFINED;
}
static const JSCFunctionListEntry file_proto[] = {
  JS_CGETSET_DEF("size", f_size, 0), JS_CGETSET_DEF("position", f_pos, 0), JS_CGETSET_DEF("path", f_path, 0),
  JS_CFUNC_DEF("read", 1, f_read), JS_CFUNC_DEF("readInto", 3, f_read_into), JS_CFUNC_DEF("text", 1, f_text),
  JS_CFUNC_DEF("seek", 1, f_seek), JS_CFUNC_DEF("close", 0, f_close),
};
static JSValue wrap(JSContext *c, JSClassID k, uint32_t id) {
  JSValue o = JS_NewObjectClass(c, (int)k);
  if (JS_IsException(o)) {jsrt_handle_close(id); return o;}
  JS_SetOpaque(o, (void *)(uintptr_t)id);
  return o;
}
static JSValue js_open(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char path[96]; struct entry e;
  if (jsrt_need(c, ANY_READ, "fs.open") || jsrt_copy_str(c, a[0], path, sizeof path)) return JS_EXCEPTION;
  const char *why;
  if (jsrt_path_check(path, 0, &why)) return fail(c, "EACCES", "fs.open: path refused, %s: %s", why, path);
  int r = stat_path(path, &e);
  if (r == -2) return fail(c, "ENOENT", "fs.open: no file %s", path);
  if (r) return fail_errno(c, r, "fs.open", path);
  if (e.dir) return fail(c, "EINVAL", "fs.open: %s is a folder (use fs.readdir)", path);
  struct fobj *f = calloc(1, sizeof *f);
  if (!f) return JS_ThrowOutOfMemory(c);
  snprintf(f->path, sizeof f->path, "%s", path); f->size = e.size;
  r = plat_fopen(path, &f->fd);
  if (r) {free(f); return r == -2 ? fail(c, "ENOENT", "fs.open: no file %s", path) : fail_errno(c, r, "fs.open", path);}
  uint32_t id = jsrt_handle_open(JSRT_H_FILE, fobj_release, f);
  if (!id) {fobj_release(f); return JS_ThrowRangeError(c, "fs.open: too many open handles (%u); close one first", JSRT_H_MAX);}
  return wrap(c, file_class, id);
}

/* ---- the staged writer ---- */
static int name83(const char *s, size_t n, int allow_dot) { /* the kernel's fatput_name: A-Z 0-9 _ - ; 1-8 then . and 1-3 */
  size_t base = 0, ext = 0; int dot = 0;
  for (size_t i = 0; i < n; i++) {
    char ch = s[i];
    if (ch == '.') {if (!allow_dot || dot || !base) return -1; dot = 1; continue;}
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return -1;
    if (dot ? ++ext > 3 : ++base > 8) return -2;
  }
  return base && (!dot || ext) ? 0 : -1;
}
/* Upcases path into out (the card is case-blind; the kernel wants canonical uppercase) and checks the kernel's write names:
   /sd/OS/CACHE/NAME or /sd/OS/SAVES/GAME/NAME. 0, or -1 with *code and *why. */
static int write_target(const char *path, char *out, const char **code, const char **why) {
  size_t n = strlen(path);
  for (size_t i = 0; i <= n; i++) out[i] = path[i] >= 'a' && path[i] <= 'z' ? (char)(path[i] - 32) : path[i];
  if (!strncmp(out, "/SD/OS/CACHE/", 13)) {
    out[1] = 's'; out[2] = 'd';
    int r = name83(out + 13, n - 13, 1);
    if (r) {*code = r == -2 ? "ENAMETOOLONG" : "EINVAL"; *why = "a cache name is 8.3: up to 8 letters, digits, _ or -, a dot and up to 3 more"; return -1;}
    return 0;
  }
  if (!strncmp(out, "/SD/OS/SAVES/", 13)) {
    out[1] = 's'; out[2] = 'd';
    const char *g = out + 13, *slash = strchr(g, '/');
    if (!slash || strchr(slash + 1, '/')) {*code = "EINVAL"; *why = "a save is /sd/OS/SAVES/<id>/NAME.EXT (one folder, no deeper)"; return -1;}
    int r = name83(g, (size_t)(slash - g), 0), s = name83(slash + 1, strlen(slash + 1), 1);
    if (r || s) {*code = r == -2 || s == -2 ? "ENAMETOOLONG" : "EINVAL"; *why = "a save name is 8.3: up to 8 letters, digits, _ or -, a dot and up to 3 more"; return -1;}
    return 0;
  }
  *code = "EROFS"; *why = "writes go to /sd/OS/SAVES/<id>/ or /sd/OS/CACHE/ only"; return -1;
}
static JSValue js_create_writer(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char path[96], canon[96]; const char *why, *code;
  if (jsrt_need(c, ANY_WRITE, "fs.write") || jsrt_copy_str(c, a[0], path, sizeof path)) return JS_EXCEPTION;
  if (jsrt_path_check(path, 1, &why)) return fail(c, strstr(why, "capability") ? "EACCES" : "EROFS", "fs.write: %s: %s", why, path);
  if (write_target(path, canon, &code, &why)) return fail(c, code, "fs.write: %s: %s", why, path);
  if (writer_open) return fail(c, "EBUSY", "fs.write: another writer is open (the kernel has one); commit or abort it first");
  struct fobj *f = calloc(1, sizeof *f);
  if (!f) return JS_ThrowOutOfMemory(c);
  f->writer = 1; snprintf(f->path, sizeof f->path, "%s", canon);
  uint32_t id = jsrt_handle_open(JSRT_H_FILE, fobj_release, f);
  if (!id) {free(f); return JS_ThrowRangeError(c, "fs.write: too many open handles (%u); close one first", JSRT_H_MAX);}
  writer_open = 1; /* released with the handle */
  return wrap(c, writer_class, id);
}
static JSValue w_size(JSContext *c, JSValueConst t) {struct fobj *f = get(c, t, writer_class); return f ? JS_NewUint32(c, f->n) : JS_EXCEPTION;}
static JSValue w_path(JSContext *c, JSValueConst t) {struct fobj *f = get(c, t, writer_class); return f ? JS_NewString(c, f->path) : JS_EXCEPTION;}
/* write(data) -> bytes staged so far. data: a string (UTF-8), an ArrayBuffer or a typed array. Over quota: EDQUOT, nothing added. */
static JSValue w_write(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  struct fobj *f = get(c, t, writer_class); const uint8_t *p; size_t len; const char *str = 0;
  if (!f || n < 1) return f ? JS_ThrowTypeError(c, "write(data) needs data") : JS_EXCEPTION;
  if (JS_IsString(a[0])) {str = JS_ToCStringLen(c, &len, a[0]); p = (const uint8_t *)str; if (!str) return JS_EXCEPTION;}
  else {uint8_t *q; if (bytes_of(c, a[0], &q, &len)) return JS_EXCEPTION; p = q;}
  JSValue r = JS_UNDEFINED;
  if ((uint64_t)f->n + len > jsrt_write_quota) r = fail(c, "EDQUOT", "this app may stage %u bytes in a save file (%u already, %u more asked)", (unsigned)jsrt_write_quota, (unsigned)f->n, (unsigned)len);
  else if ((uint64_t)f->n + len > JSFS_KERNEL_MAX) r = fail(c, "ENOSPC", "the kernel writes at most 1 MiB in one file (%u already, %u more asked)", (unsigned)f->n, (unsigned)len);
  else if (f->n + len > f->cap) {
    uint32_t cap = f->cap ? f->cap : 1024; while (cap < f->n + len) cap *= 2;
    uint8_t *nb = realloc(f->buf, cap);
    if (!nb) r = JS_ThrowOutOfMemory(c); else {f->buf = nb; f->cap = cap;}
  }
  if (JS_IsUndefined(r)) {if (len) memcpy(f->buf + f->n, p, len); f->n += (uint32_t)len; r = JS_NewUint32(c, f->n);}
  if (str) JS_FreeCString(c, str);
  return r;
}
struct sum {const char *skip; size_t skip_n; uint64_t total;};
static void sum_cb(void *user, const char *name, uint32_t n, int dir, uint32_t size, uint32_t modified, uint32_t created) {
  struct sum *s = user; (void)modified; (void)created;
  if (dir || (n == s->skip_n && ieq_n(name, s->skip, n))) return;
  s->total += size;
}
/* commit() publishes the staged bytes with the kernel's whole-file write and closes the writer. On an error the writer
   stays open (the app may retry, for EBUSY, or abort). close() is the same as commit(). */
static JSValue w_commit(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)n; (void)a;
  struct fobj *f = get(c, t, writer_class);
  if (!f) return JS_EXCEPTION;
  char folder[96]; const char *slash = strrchr(f->path, '/'); size_t fl = (size_t)(slash - f->path);
  memcpy(folder, f->path, fl); folder[fl] = 0;
  struct sum s = {slash + 1, strlen(slash + 1), 0};
  plat_list_meta(folder, 0, sum_cb, &s); /* a folder that does not exist yet holds nothing */
  if (s.total + f->n > jsrt_write_quota) return fail(c, "EDQUOT", "this app may hold %u bytes in its save folder (%u there, %u more committed)", (unsigned)jsrt_write_quota, (unsigned)s.total, (unsigned)f->n);
  int r = plat_write_file(f->path, f->buf, f->n);
  if (r) return fail_errno(c, r, "commit", f->path);
  uint32_t bytes = f->n;
  jsrt_handle_close((uint32_t)(uintptr_t)JS_GetOpaque(t, writer_class));
  return JS_NewUint32(c, bytes);
}
/* abort() discards the staged bytes and closes the writer; calling it again does nothing. */
static JSValue w_abort(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)c; (void)n; (void)a;
  uint32_t id = (uint32_t)(uintptr_t)JS_GetOpaque(t, writer_class);
  if (id) jsrt_handle_close(id);
  return JS_UNDEFINED;
}
static const JSCFunctionListEntry writer_proto[] = {
  JS_CGETSET_DEF("size", w_size, 0), JS_CGETSET_DEF("path", w_path, 0),
  JS_CFUNC_DEF("write", 1, w_write), JS_CFUNC_DEF("commit", 0, w_commit), JS_CFUNC_DEF("close", 0, w_commit), JS_CFUNC_DEF("abort", 0, w_abort),
};
static JSValue js_quota(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; (void)a; return JS_NewUint32(c, jsrt_write_quota);}

static void new_class(JSRuntime *rt, JSContext *c, JSClassID *id, const char *name, const JSCFunctionListEntry *fns, int n) {
  JS_NewClassID(rt, id);
  JSClassDef def = {name, fobj_finalizer, 0, 0, 0};
  JS_NewClass(rt, *id, &def);
  JSValue proto = JS_NewObject(c);
  JS_SetPropertyFunctionList(c, proto, fns, n);
  JS_SetClassProto(c, *id, proto);
}
static const JSCFunctionListEntry fs_fns[] = {
  JS_CFUNC_DEF("open", 1, js_open), JS_CFUNC_DEF("stat", 1, js_stat), JS_CFUNC_DEF("readdirMeta", 1, js_readdir_meta),
  JS_CFUNC_DEF("createWriter", 1, js_create_writer), JS_CFUNC_DEF("writeQuota", 0, js_quota),
};
void jsfs_init(JSRuntime *rt, JSContext *c, JSValueConst native) {
  writer_open = 0;
  new_class(rt, c, &file_class, "FileHandle", file_proto, (int)(sizeof file_proto / sizeof file_proto[0]));
  new_class(rt, c, &writer_class, "FileWriter", writer_proto, (int)(sizeof writer_proto / sizeof writer_proto[0]));
  JS_SetPropertyFunctionList(c, native, fs_fns, (int)(sizeof fs_fns / sizeof fs_fns[0]));
}
