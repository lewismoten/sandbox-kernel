/* SPDX-License-Identifier: MIT; e-ink for apps (slice 4d), see jseink.h. One JSRT_H_EINK entry holds the state; natives take its id
 * so a closed panel is EBADF. The runtime paces itself by the kernel's limits (min interval, updates per minute) so it normally
 * never asks too soon; a kernel EAGAIN is still honored. An update (page, title or clear) that cannot go yet replaces any queued one:
 * only the newest is kept, then sent by the pump. release at stop clears the panel back to the kernel's screen if it can. */
#include "jseink.h"
#include "app.h"
#include "handle.h"
#include "jsdev.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include "alloc.h"

#define PAGE 3904u
#define WINDOW_MAX 64u
enum {Q_NONE, Q_PAGE, Q_TITLE, Q_CLEAR};
struct ei {
  uint32_t id; struct plat_eink_info info;
  int pend, keep; uint8_t page[PAGE]; char name[17], build[17]; uint8_t mark[200]; int has_mark;
  int has_last; uint64_t last, retry_at, stamps[WINDOW_MAX]; unsigned nst;
  uint64_t sent, queued, replaced, eagain; int err;
};
static struct ei *E;

static const char *err_name(int e) {return e == -16 ? "EBUSY" : e == -19 ? "ENODEV" : e == -13 ? "EACCES" : e == -9 ? "EBADF" : e == -22 ? "EINVAL" : e == -11 ? "EAGAIN" : "EIO";}
static JSValue throw_errno(JSContext *c, int e, const char *what) {return JS_ThrowPlainError(c, "%s: %s failed (%d)", err_name(e), what, e);}
static uint64_t allowed_at(const struct ei *e, uint64_t now);
static void ei_release(void *u) { /* stop or release(): the kernel gets its screen back if an update may go now; close() (keep) leaves the page up */
  struct ei *e = u; uint64_t now = plat_clock_ns();
  if (!e->keep && allowed_at(e, now) <= now) plat_eink_clear();
  plat_eink_close(); free(E); E = 0;
}
static struct ei *get(JSContext *c, JSValueConst v) {
  uint32_t id = 0;
  if (JS_ToUint32(c, &id, v)) return 0;
  struct ei *e = E && E->id == id ? jsrt_handle_get(id, JSRT_H_EINK) : 0;
  if (!e) JS_ThrowPlainError(c, "EBADF: the e-ink panel is closed");
  return e;
}
/* When may the kernel take an update? The min interval after the last accepted one, and max per 60 s over a sliding window. */
static uint64_t allowed_at(const struct ei *e, uint64_t now) {
  uint64_t at = e->has_last ? e->last + e->info.min_interval_ms * 1000000ull : 0;
  unsigned in = 0; uint64_t oldest = 0, win = 60ull * 1000000000ull;
  for (unsigned i = 0; i < e->nst; i++) if (now < e->stamps[i] + win) {if (!in || e->stamps[i] < oldest) oldest = e->stamps[i]; in++;}
  if (e->info.max_per_minute && in >= e->info.max_per_minute && oldest + win > at) at = oldest + win;
  return at;
}
static void accepted(struct ei *e, uint64_t now) {
  e->last = now; e->has_last = 1; e->sent++;
  unsigned lim = e->info.max_per_minute < WINDOW_MAX ? (unsigned)e->info.max_per_minute : WINDOW_MAX;
  if (!lim) return;
  uint64_t win = 60ull * 1000000000ull; unsigned k = 0;
  for (unsigned i = 0; i < e->nst; i++) if (now < e->stamps[i] + win) e->stamps[k++] = e->stamps[i];
  if (k >= lim) {memmove(e->stamps, e->stamps + 1, (k - 1) * sizeof e->stamps[0]); k--;}
  e->stamps[k++] = now; e->nst = k;
}
/* Sends the queued update if the limits allow: 0 sent (or nothing queued), -11 not yet (retry_at says when), other errno: dropped. */
static int try_send(struct ei *e, uint64_t now) {
  if (!e->pend) return 0;
  uint64_t at = allowed_at(e, now);
  if (now < at) {e->retry_at = at; return -11;}
  int r = e->pend == Q_PAGE ? plat_eink_page(e->page)
        : e->pend == Q_CLEAR ? plat_eink_clear()
        : plat_eink_title(e->name, (uint32_t)strlen(e->name), e->has_mark ? e->mark : 0, e->build[0] ? e->build : 0);
  if (r == -11) {e->eagain++; e->retry_at = now + JSEINK_RETRY_NS; return -11;}
  e->pend = Q_NONE;
  if (r) {e->err = r; return r;}
  accepted(e, now); return 0;
}
void jseink_pump(uint64_t now) {
  struct ei *e = E;
  if (e && e->pend && now >= e->retry_at) try_send(e, now);
}
uint64_t jseink_wake(uint64_t now, uint64_t wake) {
  if (E && E->pend) {uint64_t w = E->retry_at > now ? E->retry_at : now + 1; if (w < wake) wake = w;}
  return wake;
}
/* Queues an update (the newest replaces an older one) and tries to send it at once. Returns 1 sent, 0 queued, or a negative errno. */
static int submit(struct ei *e, int kind) {
  if (e->pend) e->replaced++;
  e->pend = kind; e->err = 0; e->queued++;
  int r = try_send(e, plat_clock_ns());
  if (r == 0) {e->queued--; return 1;}
  return r == -11 ? 0 : r;
}

static JSValue js_probe(JSContext *c, JSValueConst t, int n, JSValueConst *v) { /* is there a panel? */
  (void)t; (void)n; (void)v;
  if (jsrt_need(c, APP_CAP_EINK, "eink.available")) return JS_EXCEPTION;
  if (E) return JS_TRUE;
  int r = plat_eink_open();
  if (r == 0) plat_eink_close();
  return JS_NewBool(c, r == 0 || r == -16);
}
static JSValue js_open(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; (void)n; (void)v;
  if (jsrt_need(c, APP_CAP_EINK, "eink.open")) return JS_EXCEPTION;
  if (E) return JS_ThrowPlainError(c, "EBUSY: this app already has the e-ink panel open");
  struct ei *e = calloc(1, sizeof *e);
  if (!e) return JS_ThrowOutOfMemory(c);
  int r = plat_eink_open();
  if (r == -19) {free(e); return JS_NULL;} /* no panel: eink.open() gives null and eink.available is false */
  if (r) {free(e); return throw_errno(c, r, "open /dev/eink");}
  r = plat_eink_query(&e->info);
  if (r || e->info.page_bytes != PAGE) {plat_eink_close(); free(e); return r ? throw_errno(c, r, "query /dev/eink") : JS_ThrowPlainError(c, "EINVAL: the panel's page is not 3904 bytes");}
  e->id = jsrt_handle_open(JSRT_H_EINK, ei_release, e);
  if (!e->id) {plat_eink_close(); free(e); return JS_ThrowRangeError(c, "EMFILE: too many open handles");}
  E = e;
  return JS_NewUint32(c, e->id);
}
static JSValue js_info(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct ei *e = n > 0 ? get(c, v[0]) : 0;
  if (!e) return JS_EXCEPTION;
  JSValue o = JS_NewObject(c);
#define U(k, x) JS_SetPropertyStr(c, o, k, JS_NewInt64(c, (int64_t)(x)))
  U("width", e->info.width); U("height", e->info.height); U("pageBytes", e->info.page_bytes); U("titleMax", e->info.title_max);
  U("markBytes", e->info.mark_bytes); U("minIntervalMs", e->info.min_interval_ms); U("maxPerMinute", e->info.max_per_minute);
  return o;
}
static JSValue result(JSContext *c, int r, const char *what) {return r < 0 && r != -11 ? throw_errno(c, r, what) : JS_NewInt32(c, r);}
/* page(id, Uint8Array(3904)) -> 1 sent now, 0 queued (the newest page is kept; the pump sends it when the limits allow) */
static JSValue js_page(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct ei *e = n > 0 ? get(c, v[0]) : 0;
  if (!e) return JS_EXCEPTION;
  if (n < 2 || JS_GetTypedArrayType(v[1]) != JS_TYPED_ARRAY_UINT8) return JS_ThrowTypeError(c, "eink.page: a Uint8Array of 3904 bytes");
  size_t off, len, bpe; JSValue ab = JS_GetTypedArrayBuffer(c, v[1], &off, &len, &bpe);
  if (JS_IsException(ab)) return ab;
  size_t asz; uint8_t *base = JS_GetArrayBuffer(c, &asz, ab);
  JS_FreeValue(c, ab);
  if (!base || off + len > asz) return JS_ThrowPlainError(c, "EBADF: the page buffer was detached");
  if (len != PAGE) return JS_ThrowRangeError(c, "EINVAL: a page is exactly %u bytes (250x122, 32-byte rows), not %u", PAGE, (unsigned)len);
  memcpy(e->page, base + off, PAGE);
  int r = submit(e, Q_PAGE);
  return result(c, r, "write /dev/eink");
}
/* title(id, name, mark|undefined, build|undefined) */
static JSValue js_title(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct ei *e = n >= 4 ? get(c, v[0]) : 0;
  if (!e) return n >= 4 ? JS_EXCEPTION : JS_ThrowTypeError(c, "eink: title(id, name, mark, build)");
  const char *name = JS_ToCString(c, v[1]);
  if (!name) return JS_EXCEPTION;
  size_t nl = strlen(name);
  int bad = nl < 1 || nl > 16;
  for (size_t i = 0; i < nl && !bad; i++) {char ch = name[i]; bad = !((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || strchr(" -:./()=,_+!?';%", ch));}
  if (bad) {JS_FreeCString(c, name); return JS_ThrowRangeError(c, "EINVAL: a title is 1 to 16 characters from A-Z, 0-9, space and -:./()=,_+!?';%%");}
  memcpy(e->name, name, nl); e->name[nl] = 0; JS_FreeCString(c, name);
  e->has_mark = 0; e->build[0] = 0;
  if (!JS_IsUndefined(v[2]) && !JS_IsNull(v[2])) {
    size_t off, len, bpe; JSValue ab = JS_GetTypedArrayBuffer(c, v[2], &off, &len, &bpe);
    if (JS_IsException(ab)) return ab;
    size_t asz; uint8_t *base = JS_GetArrayBuffer(c, &asz, ab);
    JS_FreeValue(c, ab);
    if (!base || off + len > asz || len != 200 || JS_GetTypedArrayType(v[2]) != JS_TYPED_ARRAY_UINT8) return JS_ThrowRangeError(c, "EINVAL: a mark is a Uint8Array of 200 bytes (40x40)");
    memcpy(e->mark, base + off, 200); e->has_mark = 1;
  }
  if (!JS_IsUndefined(v[3]) && !JS_IsNull(v[3])) {
    const char *b = JS_ToCString(c, v[3]);
    if (!b) return JS_EXCEPTION;
    strncpy(e->build, b, 16); e->build[16] = 0; JS_FreeCString(c, b);
  }
  return result(c, submit(e, Q_TITLE), "title /dev/eink");
}
static JSValue js_clear(JSContext *c, JSValueConst t, int n, JSValueConst *v) { /* back to the kernel's screen */
  (void)t; struct ei *e = n > 0 ? get(c, v[0]) : 0;
  if (!e) return JS_EXCEPTION;
  return result(c, submit(e, Q_CLEAR), "clear /dev/eink");
}
static JSValue js_stats(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct ei *e = n > 0 ? get(c, v[0]) : 0;
  if (!e) return JS_EXCEPTION;
  uint64_t now = plat_clock_ns(), at = allowed_at(e, now);
  JSValue o = JS_NewObject(c);
#define U(k, x) JS_SetPropertyStr(c, o, k, JS_NewInt64(c, (int64_t)(x)))
  U("sent", e->sent); U("queued", e->queued); U("replaced", e->replaced); U("eagain", e->eagain);
  U("pending", e->pend != Q_NONE); U("nextInMs", at > now ? (at - now + 999999ull) / 1000000ull : 0);
  return o;
#undef U
}
static JSValue js_close(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; uint32_t id = 0; int clear = n > 1 && JS_ToBool(c, v[1]);
  if (n > 0 && JS_ToUint32(c, &id, v[0])) return JS_EXCEPTION;
  if (!E || E->id != id) return JS_UNDEFINED; /* closing twice is fine */
  E->pend = Q_NONE; E->keep = !clear; /* release (clear): the kernel's screen returns now if the limits allow; close keeps the page up. A queued page is dropped either way */
  jsrt_handle_close(id);
  return JS_UNDEFINED;
}
static const JSCFunctionListEntry fns[] = {
  JS_CFUNC_DEF("einkProbe", 0, js_probe), JS_CFUNC_DEF("einkOpen", 0, js_open), JS_CFUNC_DEF("einkInfo", 1, js_info),
  JS_CFUNC_DEF("einkPage", 2, js_page), JS_CFUNC_DEF("einkTitle", 4, js_title), JS_CFUNC_DEF("einkClear", 1, js_clear),
  JS_CFUNC_DEF("einkStats", 1, js_stats), JS_CFUNC_DEF("einkClose", 2, js_close),
};
void jseink_init(JSContext *c, JSValueConst native) {
  E = 0;
  JS_SetPropertyFunctionList(c, native, fns, (int)(sizeof fns / sizeof fns[0]));
}
