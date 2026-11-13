/* SPDX-License-Identifier: MIT; power for apps (slice 4d), see jspower.h. Holds are kept in a small table here (id, flags, reason, the
 * kernel's token); the app sees the id only through a JS object. Releasing an id twice is harmless. A kernel older than 1.12 answers
 * the blank query with EINVAL: the screen is then treated as lit. */
#include "jspower.h"
#include "app.h"
#include "jsdev.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>

#define HOLDS 16u
#define REASON 40u
static JSContext *pctx;
struct hold {uint32_t id, flags; uint64_t token; char reason[REASON + 1];};
static struct hold H[HOLDS];
static uint32_t next_id;
static unsigned nh;
static uint32_t bits;            /* the last asleep bits the kernel gave (bit 0: screen blank, bit 1: slowed) */
static uint64_t checked;         /* clock of that look; 0 = never */
static int watching, was_dark;

static const char *err_name(int e) {return e == -24 ? "EMFILE" : e == -22 ? "EINVAL" : e == -9 ? "EBADF" : e == -75 ? "EOVERFLOW" : "EIO";}
static uint32_t held_flags(void) {uint32_t f = 0; for (unsigned i = 0; i < nh; i++) f |= H[i].flags; return f;}
static void look(uint64_t now) {
  if (checked && now - checked < JSPOWER_POLL_NS) return;
  uint32_t b = 0;
  if (plat_power_asleep(&b)) b = 0; /* an older kernel or an error: lit */
  bits = b; checked = now;
}
int jspower_dark(uint64_t now) {look(now); return (bits & 1u) && !(held_flags() & 1u);}

/* hold(flags, reason) -> id; 1 = no blank, 2 = no slowdown */
static JSValue js_hold(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; uint32_t flags = 1;
  if (jsrt_need(c, APP_CAP_POWER, "power.hold")) return JS_EXCEPTION;
  if (n < 2 || JS_ToUint32(c, &flags, v[0])) return n < 2 ? JS_ThrowTypeError(c, "power.hold(flags, reason)") : JS_EXCEPTION;
  if (!flags || flags & ~3u) return JS_ThrowRangeError(c, "EINVAL: a hold needs noBlank and/or noSlowdown");
  const char *r = JS_ToCString(c, v[1]);
  if (!r) return JS_EXCEPTION;
  if (nh == HOLDS) {JS_FreeCString(c, r); return JS_ThrowRangeError(c, "EMFILE: %u holds are already taken", HOLDS);}
  uint64_t tok = 0;
  int e = plat_power_acquire(flags, &tok);
  if (e) {JS_FreeCString(c, r); return JS_ThrowPlainError(c, "%s: power_hold failed (%d)", err_name(e), e);}
  struct hold *h = &H[nh++];
  h->id = ++next_id; h->flags = flags; h->token = tok;
  snprintf(h->reason, sizeof h->reason, "%.*s", (int)REASON, r); JS_FreeCString(c, r);
  return JS_NewUint32(c, h->id);
}
static int drop(uint32_t id) { /* 1 when it was held */
  for (unsigned i = 0; i < nh; i++) if (H[i].id == id) {
    plat_power_release(H[i].token);
    memmove(&H[i], &H[i + 1], (nh - i - 1) * sizeof H[0]); nh--;
    checked = 0; return 1;
  }
  return 0;
}
static JSValue js_release(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; uint32_t id = 0;
  if (n < 1 || JS_ToUint32(c, &id, v[0])) return JS_EXCEPTION;
  return JS_NewBool(c, drop(id));
}
static JSValue js_list(JSContext *c, JSValueConst t, int n, JSValueConst *v) { /* [[id, flags, reason], ...] */
  (void)t; (void)n; (void)v; JSValue a = JS_NewArray(c);
  for (unsigned i = 0; i < nh; i++) {
    JSValue e = JS_NewArray(c);
    JS_SetPropertyUint32(c, e, 0, JS_NewUint32(c, H[i].id)); JS_SetPropertyUint32(c, e, 1, JS_NewUint32(c, H[i].flags));
    JS_SetPropertyUint32(c, e, 2, JS_NewString(c, H[i].reason)); JS_SetPropertyUint32(c, a, i, e);
  }
  return a;
}
static JSValue js_state(JSContext *c, JSValueConst t, int n, JSValueConst *v) { /* asleep bits, looked up now */
  (void)t; (void)n; (void)v;
  if (jsrt_need(c, APP_CAP_POWER, "power.asleep")) return JS_EXCEPTION;
  checked = 0; look(plat_clock_ns());
  return JS_NewUint32(c, bits);
}
static JSValue js_watch(JSContext *c, JSValueConst t, int n, JSValueConst *v) { /* watch(on): a 'blank'/'wake' listener exists */
  (void)t;
  if (jsrt_need(c, APP_CAP_POWER, "power.on")) return JS_EXCEPTION;
  int on = n > 0 && JS_ToBool(c, v[0]);
  if (on && !watching) {checked = 0; look(plat_clock_ns()); was_dark = jspower_dark(plat_clock_ns());}
  watching = on; return JS_UNDEFINED;
}
void jspower_pump(uint64_t now) {
  if (!watching) return;
  int d = jspower_dark(now);
  if (d == was_dark) return;
  was_dark = d;
  JSValue a = JS_NewBool(pctx, d);
  JS_FreeValue(pctx, jsrt_call_native("onPower", 1, &a));
}
uint64_t jspower_wake(uint64_t now, uint64_t wake) {
  if (watching) {uint64_t w = checked + JSPOWER_POLL_NS; if (w <= now) w = now + 1; if (w < wake) wake = w;}
  return wake;
}
unsigned jspower_release_all(void) {
  unsigned n = nh;
  while (nh) drop(H[0].id);
  return n;
}
static const JSCFunctionListEntry fns[] = {
  JS_CFUNC_DEF("powerHold", 2, js_hold), JS_CFUNC_DEF("powerRelease", 1, js_release), JS_CFUNC_DEF("powerList", 0, js_list),
  JS_CFUNC_DEF("powerState", 0, js_state), JS_CFUNC_DEF("powerWatch", 1, js_watch),
};
void jspower_init(JSContext *c, JSValueConst native) {
  pctx = c; nh = 0; checked = 0; bits = 0; watching = was_dark = 0;
  JS_SetPropertyFunctionList(c, native, fns, (int)(sizeof fns / sizeof fns[0]));
}
