/* SPDX-License-Identifier: MIT; display, time and input for apps (slice 4b), see jsdev.h and docs/apps.md.
 * Display: the mode list and rule, and the surface lease (one JSRT_H_DISPLAY entry): typed pixels over the kernel's mapped
 * surface, present, an 8 bpp palette; the DOM-lite UI stops drawing while it is held. Time: the microsecond clock,
 * sleep(ms) promises (one JSRT_H_TIMER entry holds them; the loop wakes for them in chunks) and Math.random from a seed.
 * Input: raw queues (JSRT_H_INPUT entries) for key, attach, detach and reset events; the document's key events stay as they are. */
#include "jsdev.h"
#include "app.h"
#include "dom.h"
#include "handle.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include "alloc.h"

#define KEYS 768u /* the kernel's key code space (SBX_INPUT_KEYS) */
#define SLEEPS_MAX 64u
#define QUEUES_MAX 4u
static JSContext *dctx;
static JSClassID lease_class, queue_class;

/* ---- the surface lease ---- */
struct lease {JSValue buf; uint32_t w, h, d, pitch; int changed;};
static int lease_on;
static uint32_t lease_id;
static uint8_t pal6[768]; /* the palette last sent, as the kernel takes it (6 bits a channel) */
int jsdev_leased(void) {return lease_on;}
static int mode_ok(uint32_t w, uint32_t h, uint32_t d) { /* the kernel's rule (docs/os-authors.md, FB_SET_MODE) */
  return (d == 32 || d == 16 || d == 8) && w >= 16 && w <= 1920 && h >= 16 && h <= 1200 && w % 4 == 0 && (uint64_t)w * h * (d / 8) <= 1228800u;
}
static JSValue make_typed(JSContext *c, const char *ctor, JSValueConst buf) {
  JSValue g = JS_GetGlobalObject(c), k = JS_GetPropertyStr(c, g, ctor);
  JSValue a[1] = {buf}, r = JS_CallConstructor(c, k, 1, a);
  JS_FreeValue(c, k); JS_FreeValue(c, g);
  return r;
}
static void lease_release(void *u) {
  struct lease *l = u;
  if (dctx && JS_IsObject(l->buf)) {JS_DetachArrayBuffer(dctx, l->buf); JS_FreeValue(dctx, l->buf);} /* a view kept past release has length 0 */
  lease_on = 0; lease_id = 0;
  if (l->changed) jsrt_restore_page_mode();
  uint32_t w, h, d; jsrt_page_mode(&w, &h, &d);
  struct dom_rect all = {0, 0, (int32_t)w, (int32_t)h}; dom_damage(&all); /* the page paints itself over what was drawn */
  free(l);
}
static void lease_finalizer(JSRuntime *r, JSValueConst v) {
  (void)r; uintptr_t id = (uintptr_t)JS_GetOpaque(v, lease_class);
  if (id) jsrt_handle_close((uint32_t)id);
}
static struct lease *lease_of(JSContext *c, JSValueConst t) {
  if (JS_GetClassID(t) != lease_class) {JS_ThrowTypeError(c, "ScreenLease object expected"); return 0;}
  uintptr_t id = (uintptr_t)JS_GetOpaque(t, lease_class);
  struct lease *l = id ? jsrt_handle_get((uint32_t)id, JSRT_H_DISPLAY) : 0;
  if (!l && !JS_HasException(c)) JS_ThrowPlainError(c, "EBADF: the screen lease was released");
  return l;
}
static JSValue get_opt(JSContext *c, JSValueConst o, const char *k, uint32_t *v) {
  JSValue p = JS_GetPropertyStr(c, o, k);
  if (JS_IsUndefined(p)) return JS_UNDEFINED;
  int r = JS_ToUint32(c, v, p); JS_FreeValue(c, p);
  return r ? JS_EXCEPTION : JS_UNDEFINED;
}
static JSValue js_lease(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; if (jsrt_need(c, APP_CAP_DISPLAY, "screen.lease")) return JS_EXCEPTION;
  if (lease_on) return JS_ThrowPlainError(c, "EBUSY: the screen is already leased; release() it first");
  if (jsrt_game_active()) return JS_ThrowPlainError(c, "EBUSY: a <game> is playing; stop it before leasing the screen");
  uint32_t w, h, d; jsrt_page_mode(&w, &h, &d);
  uint32_t pw = w, ph = h, pd = d;
  if (n > 0 && JS_IsObject(a[0]) && (JS_IsException(get_opt(c, a[0], "width", &w)) || JS_IsException(get_opt(c, a[0], "height", &h)) ||
                                     JS_IsException(get_opt(c, a[0], "depth", &d)))) return JS_EXCEPTION;
  if (!mode_ok(w, h, d)) return JS_ThrowRangeError(c, "screen.lease: %ux%ux%u is not a mode the kernel takes (screen.modes() lists them)", (unsigned)w, (unsigned)h, (unsigned)d);
  int changed = w != pw || h != ph || d != pd;
  uint32_t us;
  if (changed && plat_set_mode(w, h, d, &us)) return JS_ThrowRangeError(c, "screen.lease: the kernel refused %ux%ux%u", (unsigned)w, (unsigned)h, (unsigned)d);
  struct lease *l = calloc(1, sizeof *l);
  uint32_t id = l ? jsrt_handle_open(JSRT_H_DISPLAY, lease_release, l) : 0;
  if (!id) {
    free(l); if (changed) jsrt_restore_page_mode();
    return l ? JS_ThrowRangeError(c, "EMFILE: too many open handles") : JS_ThrowOutOfMemory(c);
  }
  l->w = w; l->h = h; l->d = d; l->pitch = w * d / 8; l->changed = changed;
  lease_on = 1; lease_id = id;
  if (d == 8) { /* defined colors: a gray ramp until the app sets its own */
    for (unsigned i = 0; i < 256; i++) pal6[i * 3] = pal6[i * 3 + 1] = pal6[i * 3 + 2] = (uint8_t)(i >> 2);
    plat_palette(pal6);
  }
  l->buf = JS_NewArrayBuffer(c, plat_fb(), (size_t)l->pitch * h, 0, 0, 0, 0);
  JSValue px = make_typed(c, d == 32 ? "Uint32Array" : d == 16 ? "Uint16Array" : "Uint8Array", l->buf);
  JSValue o = JS_NewObjectClass(c, (int)lease_class);
  if (JS_IsException(l->buf) || JS_IsException(px) || JS_IsException(o)) {JS_FreeValue(c, px); JS_FreeValue(c, o); l->buf = JS_UNDEFINED; jsrt_handle_close(id); return JS_EXCEPTION;}
  JS_SetOpaque(o, (void *)(uintptr_t)id);
  JS_SetPropertyStr(c, o, "width", JS_NewUint32(c, w)); JS_SetPropertyStr(c, o, "height", JS_NewUint32(c, h));
  JS_SetPropertyStr(c, o, "depth", JS_NewUint32(c, d)); JS_SetPropertyStr(c, o, "pitch", JS_NewUint32(c, l->pitch));
  JS_SetPropertyStr(c, o, "pixels", px);
  JS_SetPropertyStr(c, o, "buffer", JS_DupValue(c, l->buf));
  return o;
}
static JSValue js_lease_present(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  struct lease *l = lease_of(c, t);
  if (!l) return JS_EXCEPTION;
  uint32_t v[4] = {0, 0, l->w, l->h};
  for (int i = 0; i < 4 && i < n; i++) if (JS_ToUint32(c, &v[i], a[i])) return JS_EXCEPTION;
  if (n < 3) {v[2] = l->w - (v[0] < l->w ? v[0] : l->w); v[3] = l->h - (v[1] < l->h ? v[1] : l->h);}
  if (!v[2] || !v[3] || v[0] >= l->w || v[1] >= l->h || v[2] > l->w - v[0] || v[3] > l->h - v[1])
    return JS_ThrowRangeError(c, "present(%u, %u, %u, %u) is outside the %ux%u surface", (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3], (unsigned)l->w, (unsigned)l->h);
  plat_present(v[0], v[1], v[2], v[3]);
  return JS_UNDEFINED;
}
static JSValue js_lease_release(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)n; (void)a;
  if (JS_GetClassID(t) != lease_class) return JS_ThrowTypeError(c, "ScreenLease object expected");
  uintptr_t id = (uintptr_t)JS_GetOpaque(t, lease_class);
  if (id && jsrt_handle_close((uint32_t)id) == 0) JS_SetOpaque(t, 0);
  return JS_UNDEFINED; /* releasing twice is fine */
}
/* setPalette(rgb, first = 0): rgb is r,g,b bytes (0 to 255) from entry `first`; the screen keeps the top 6 bits of each. */
static JSValue js_lease_set_palette(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  struct lease *l = lease_of(c, t);
  if (!l) return JS_EXCEPTION;
  if (l->d != 8) return JS_ThrowTypeError(c, "setPalette: only an 8 bpp lease has a palette");
  uint32_t first = 0, len = 0;
  JSValue ln = n > 0 && JS_IsObject(a[0]) ? JS_GetPropertyStr(c, a[0], "length") : JS_UNDEFINED;
  int bad_len = JS_ToUint32(c, &len, ln); JS_FreeValue(c, ln);
  if (bad_len || !JS_IsObject(a[0])) {JS_FreeValue(c, JS_GetException(c)); return JS_ThrowTypeError(c, "setPalette(rgb): an array or typed array of r,g,b bytes");}
  if (n > 1 && JS_ToUint32(c, &first, a[1])) return JS_EXCEPTION;
  if (len % 3 || !len || first + len / 3 > 256) return JS_ThrowRangeError(c, "setPalette: a multiple of 3 values, entries %u to 255 at most", (unsigned)first);
  uint8_t tmp[768];
  for (uint32_t i = 0; i < len; i++) {
    JSValue e = JS_GetPropertyUint32(c, a[0], i); uint32_t v;
    int bad = JS_ToUint32(c, &v, e); JS_FreeValue(c, e);
    if (bad) return JS_EXCEPTION;
    if (v > 255) return JS_ThrowRangeError(c, "setPalette: values are 0 to 255");
    tmp[i] = (uint8_t)(v >> 2);
  }
  memcpy(pal6 + first * 3, tmp, len);
  if (plat_palette(pal6)) return JS_ThrowRangeError(c, "setPalette: the kernel refused the palette");
  return JS_NewUint32(c, len / 3);
}
/* getPalette(): the 256 entries as the screen shows them (6 bits widened the way the kernel does). */
static JSValue js_lease_get_palette(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)n; (void)a; struct lease *l = lease_of(c, t);
  if (!l) return JS_EXCEPTION;
  if (l->d != 8) return JS_ThrowTypeError(c, "getPalette: only an 8 bpp lease has a palette");
  uint8_t out[768];
  for (unsigned i = 0; i < 768; i++) out[i] = (uint8_t)(pal6[i] << 2 | pal6[i] >> 4);
  JSValue buf = JS_NewArrayBufferCopy(c, out, sizeof out), r = make_typed(c, "Uint8Array", buf);
  JS_FreeValue(c, buf);
  return r;
}
static JSValue js_modes(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; if (jsrt_need(c, APP_CAP_DISPLAY, "screen.modes")) return JS_EXCEPTION;
  static const uint16_t sizes[][2] = {{320, 200}, {320, 240}, {400, 300}, {640, 360}, {640, 400}, {640, 480}, {720, 480}, {800, 480}, {800, 600}, {960, 540}, {1024, 576}, {1024, 768}, {1280, 720}, {1280, 800}};
  static const uint8_t depths[] = {32, 16, 8};
  JSValue arr = JS_NewArray(c); uint32_t k = 0;
  for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    for (unsigned j = 0; j < 3; j++) {
      uint32_t w = sizes[i][0], h = sizes[i][1], d = depths[j];
      if (!mode_ok(w, h, d)) continue;
      JSValue o = JS_NewObject(c);
      JS_SetPropertyStr(c, o, "width", JS_NewUint32(c, w)); JS_SetPropertyStr(c, o, "height", JS_NewUint32(c, h));
      JS_SetPropertyStr(c, o, "depth", JS_NewUint32(c, d)); JS_SetPropertyStr(c, o, "pitch", JS_NewUint32(c, w * d / 8));
      JS_SetPropertyStr(c, o, "bytes", JS_NewUint32(c, w * h * d / 8));
      JS_SetPropertyUint32(c, arr, k++, o);
    }
  return arr;
}
static JSValue js_mode_ok(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; uint32_t v[3] = {0, 0, 32};
  for (int i = 0; i < 3 && i < n; i++) if (JS_ToUint32(c, &v[i], a[i])) return JS_EXCEPTION;
  return JS_NewBool(c, mode_ok(v[0], v[1], v[2]));
}
static JSValue js_leased(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; (void)a; return JS_NewBool(c, lease_on);}

/* ---- time ---- */
struct sleeper {uint64_t due; JSValue resolve;};
struct timer {struct sleeper s[SLEEPS_MAX]; unsigned n; uint32_t total;};
static struct timer *tm;
static uint32_t tm_id;
static void timer_release(void *u) {
  struct timer *t = u;
  for (unsigned i = 0; i < t->n; i++) if (dctx) JS_FreeValue(dctx, t->s[i].resolve);
  tm = 0; free(t);
}
static JSValue js_now_us(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; if (jsrt_need(c, APP_CAP_TIME, "sandbox.now")) return JS_EXCEPTION;
  return JS_NewInt64(c, (int64_t)(plat_clock_ns() / 1000u)); /* whole microseconds since the kernel started: exact in a double for 285 years */
}
static JSValue js_sleep(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; double ms = 0;
  if (jsrt_need(c, APP_CAP_TIME, "sandbox.sleep")) return JS_EXCEPTION;
  if (n > 0 && JS_ToFloat64(c, &ms, a[0])) return JS_EXCEPTION;
  if (!(ms >= 0) || ms > 2147483647.0) return JS_ThrowRangeError(c, "sleep(ms): 0 to 2147483647");
  if (!tm) {
    struct timer *x = calloc(1, sizeof *x);
    if (!x) return JS_ThrowOutOfMemory(c);
    tm_id = jsrt_handle_open(JSRT_H_TIMER, timer_release, x);
    if (!tm_id) {free(x); return JS_ThrowRangeError(c, "EMFILE: too many open handles");}
    tm = x;
  }
  if (tm->n == SLEEPS_MAX) return JS_ThrowRangeError(c, "EMFILE: %u sleeps are already waiting", (unsigned)SLEEPS_MAX);
  JSValue fn[2], p = JS_NewPromiseCapability(c, fn);
  if (JS_IsException(p)) return p;
  JS_FreeValue(c, fn[1]);
  tm->s[tm->n].due = plat_clock_ns() + (uint64_t)(ms * 1e6); tm->s[tm->n].resolve = fn[0]; tm->n++; tm->total++;
  return p;
}
void jsdev_sleeps_due(uint64_t now) {
  while (tm) { /* the earliest due first, so sleeps resolve in the order they ask for */
    unsigned best = tm->n;
    for (unsigned i = 0; i < tm->n; i++) if (tm->s[i].due <= now && (best == tm->n || tm->s[i].due < tm->s[best].due)) best = i;
    if (best == tm->n) return;
    JSValue f = tm->s[best].resolve; tm->s[best] = tm->s[--tm->n];
    JS_FreeValue(dctx, JS_Call(dctx, f, JS_UNDEFINED, 0, 0)); JS_FreeValue(dctx, f);
    if (tm && !tm->n) {jsrt_handle_close(tm_id); return;} /* nothing waiting: the entry goes, the next sleep() opens another */
  }
}

/* Math.random: xorshift128+ from a seed; the seed is the best the machine has (plat_entropy: on the board the clock at
   start, the app counter and a few clock reads, which is little more than timer jitter; key timings are mixed in as they come). */
static uint64_t rs[2], seed_used, app_counter;
static int rng_fixed;
static uint64_t splitmix(uint64_t *x) {uint64_t z = (*x += 0x9e3779b97f4a7c15ull); z = (z ^ z >> 30) * 0xbf58476d1ce4e5b9ull; z = (z ^ z >> 27) * 0x94d049bb133111ebull; return z ^ z >> 31;}
static void seed_with(uint64_t s) {uint64_t x = s; seed_used = s; rs[0] = splitmix(&x); rs[1] = splitmix(&x); if (!(rs[0] | rs[1])) rs[1] = 1;}
static JSValue js_random(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)c; (void)t; (void)n; (void)a;
  uint64_t s1 = rs[0]; const uint64_t s0 = rs[1];
  rs[0] = s0; s1 ^= s1 << 23; rs[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
  return JS_NewFloat64(c, (double)((rs[1] + s0) >> 11) * (1.0 / 9007199254740992.0));
}
static JSValue js_seed(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; (void)a; return JS_NewFloat64(c, (double)(seed_used & 0x1fffffffffffffull));}
static JSValue js_seed_random(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; double d = 0;
  if (n < 1 || JS_ToFloat64(c, &d, a[0])) return JS_EXCEPTION;
  seed_with((uint64_t)(int64_t)d); rng_fixed = 1; /* the app chose: input timings no longer touch it */
  return JS_UNDEFINED;
}

/* ---- input ---- */
static uint8_t down[KEYS / 8], released[KEYS / 8];
static int attached = 1;
static unsigned queues;
static uint32_t last_us(void) {return (uint32_t)(plat_clock_ns() / 1000u);}
static void queue_release(void *u) {(void)u; if (queues) queues--;}
static void queue_finalizer(JSRuntime *r, JSValueConst v) {
  (void)r; uintptr_t id = (uintptr_t)JS_GetOpaque(v, queue_class);
  if (id) jsrt_handle_close((uint32_t)id);
}
static JSValue js_input_open(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; if (jsrt_need(c, APP_CAP_INPUT, "input.open")) return JS_EXCEPTION;
  if (queues >= QUEUES_MAX) return JS_ThrowRangeError(c, "EMFILE: %u input queues are already open", (unsigned)QUEUES_MAX);
  uint32_t id = jsrt_handle_open(JSRT_H_INPUT, queue_release, 0);
  if (!id) return JS_ThrowRangeError(c, "EMFILE: too many open handles");
  queues++;
  JSValue o = JS_NewObjectClass(c, (int)queue_class);
  if (JS_IsException(o)) {jsrt_handle_close(id); return o;}
  JS_SetOpaque(o, (void *)(uintptr_t)id);
  return o;
}
static JSValue js_queue_close(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)n; (void)a;
  if (JS_GetClassID(t) != queue_class) return JS_ThrowTypeError(c, "InputQueue object expected");
  uintptr_t id = (uintptr_t)JS_GetOpaque(t, queue_class);
  if (id && jsrt_handle_close((uint32_t)id) == 0) JS_SetOpaque(t, 0);
  return JS_UNDEFINED;
}
static JSValue js_key_down(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; uint32_t k = 0;
  if (n > 0 && JS_ToUint32(c, &k, a[0])) return JS_EXCEPTION;
  return JS_NewBool(c, k < KEYS && (down[k / 8] >> (k % 8) & 1));
}
static JSValue js_keys_down(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; JSValue arr = JS_NewArray(c); uint32_t j = 0;
  for (uint32_t k = 0; k < KEYS; k++) if (down[k / 8] >> (k % 8) & 1) JS_SetPropertyUint32(c, arr, j++, JS_NewUint32(c, k));
  return arr;
}
static JSValue js_attached(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; (void)a; return JS_NewBool(c, attached);}
/* kind 0 key (code, value, time), 1 attach (time), 2 detach (time), 3 reset (reason, released codes, time): the prelude fans it out. */
static void raw(int kind, JSValue a1, JSValue a2, JSValue a3) {
  JSValue args[4] = {JS_NewInt32(dctx, kind), a1, a2, a3};
  JS_FreeValue(dctx, jsrt_call_native("onRaw", 4, args));
  for (int i = 0; i < 4; i++) JS_FreeValue(dctx, args[i]);
}
static void reset_keys(const char *reason) {
  JSValue arr = JS_UNDEFINED;
  if (queues) arr = JS_NewArray(dctx);
  uint32_t j = 0;
  for (uint32_t k = 0; k < KEYS; k++) if (down[k / 8] >> (k % 8) & 1) {if (queues) JS_SetPropertyUint32(dctx, arr, j++, JS_NewUint32(dctx, k));}
  memcpy(released, down, sizeof down); memset(down, 0, sizeof down);
  if (queues) raw(3, JS_NewString(dctx, reason), arr, JS_NewUint32(dctx, last_us()));
}
unsigned jsdev_input(uint16_t type, uint16_t code, int32_t value) {
  if (type == 1) {
    if (code < KEYS) {if (value) down[code / 8] |= (uint8_t)(1u << (code % 8)); else down[code / 8] &= (uint8_t)~(1u << (code % 8));}
    if (!rng_fixed) {rs[0] ^= plat_clock_ns() * 0x9e3779b97f4a7c15ull + code; rs[1] ^= rs[0] >> 7;}
    if (queues) raw(0, JS_NewUint32(dctx, code), JS_NewInt32(dctx, value), JS_NewUint32(dctx, last_us()));
    return 0;
  }
  if (type == 0 && code == 3) {reset_keys("dropped"); return 1;} /* SYN_DROPPED: the kernel lost events, so no key can be trusted */
  if (type == PLAT_EV_ATTACH) {attached = 1; if (queues) raw(1, JS_NewUint32(dctx, last_us()), JS_UNDEFINED, JS_UNDEFINED); return 0;}
  if (type == PLAT_EV_DETACH) {
    attached = 0;
    if (queues) raw(2, JS_NewUint32(dctx, last_us()), JS_UNDEFINED, JS_UNDEFINED);
    reset_keys("detached"); return 1;
  }
  return 0;
}
int jsdev_released_next(void) {
  for (uint32_t k = 0; k < KEYS; k++) if (released[k / 8] >> (k % 8) & 1) {released[k / 8] &= (uint8_t)~(1u << (k % 8)); return (int)k;}
  return -1;
}
uint64_t jsdev_wake(uint64_t now) {
  uint64_t wake = now + (attached ? JSDEV_POLL_NS : JSDEV_SLEEP_CHUNK_NS); /* with no device there is nothing to poll for, only a reopen to try */
  if (tm) for (unsigned i = 0; i < tm->n; i++) if (tm->s[i].due < wake) wake = tm->s[i].due;
  if (wake > now + JSDEV_SLEEP_CHUNK_NS) wake = now + JSDEV_SLEEP_CHUNK_NS;
  return wake > now ? wake : now + 1;
}

static void new_class(JSRuntime *rt, JSContext *c, JSClassID *id, const char *name, JSClassFinalizer *fin, const JSCFunctionListEntry *fns, int n) {
  JS_NewClassID(rt, id);
  JSClassDef def = {name, fin, 0, 0, 0};
  JS_NewClass(rt, *id, &def);
  JSValue proto = JS_NewObject(c);
  JS_SetPropertyFunctionList(c, proto, fns, n);
  JS_SetClassProto(c, *id, proto);
}
static const JSCFunctionListEntry lease_proto[] = {
  JS_CFUNC_DEF("present", 4, js_lease_present), JS_CFUNC_DEF("release", 0, js_lease_release),
  JS_CFUNC_DEF("setPalette", 2, js_lease_set_palette), JS_CFUNC_DEF("getPalette", 0, js_lease_get_palette),
};
static const JSCFunctionListEntry queue_proto[] = {JS_CFUNC_DEF("close", 0, js_queue_close)};
static const JSCFunctionListEntry dev_fns[] = {
  JS_CFUNC_DEF("lease", 1, js_lease), JS_CFUNC_DEF("modes", 0, js_modes), JS_CFUNC_DEF("modeOk", 3, js_mode_ok), JS_CFUNC_DEF("leased", 0, js_leased),
  JS_CFUNC_DEF("nowUs", 0, js_now_us), JS_CFUNC_DEF("sleep", 1, js_sleep),
  JS_CFUNC_DEF("random", 0, js_random), JS_CFUNC_DEF("seed", 0, js_seed), JS_CFUNC_DEF("seedRandom", 1, js_seed_random),
  JS_CFUNC_DEF("inputOpen", 0, js_input_open), JS_CFUNC_DEF("keyDown", 1, js_key_down), JS_CFUNC_DEF("keysDown", 0, js_keys_down),
  JS_CFUNC_DEF("inputAttached", 0, js_attached),
};
void jsdev_init(JSRuntime *rt, JSContext *c, JSValueConst native) {
  dctx = c; lease_on = 0; lease_id = 0; tm = 0; tm_id = 0; queues = 0; rng_fixed = 0;
  memset(down, 0, sizeof down); memset(released, 0, sizeof released);
  attached = plat_input_attached();
  int fixed = 0; uint64_t e = plat_entropy(&fixed);
  seed_with(fixed ? e : e ^ (++app_counter * 0x9e3779b97f4a7c15ull)); rng_fixed = fixed;
  new_class(rt, c, &lease_class, "ScreenLease", lease_finalizer, lease_proto, (int)(sizeof lease_proto / sizeof lease_proto[0]));
  new_class(rt, c, &queue_class, "InputQueue", queue_finalizer, queue_proto, 1);
  JS_SetPropertyFunctionList(c, native, dev_fns, (int)(sizeof dev_fns / sizeof dev_fns[0]));
}
