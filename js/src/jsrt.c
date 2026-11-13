/* SPDX-License-Identifier: MIT; jsrt.h: QuickJS-ng (third_party/quickjs-ng)
 * bound to the element tree, the game element, files and the clock. Native
 * classes: Element (one per dom_el), CSSStyle (element.style) and
 * CanvasRenderingContext2D (canvas.getContext('2d')). Everything else a page
 * sees is built in prelude.js. */
#include "jsrt.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "plat.h"
#include "gif.h"
#include "profile.h"
#include "app.h"
#include "handle.h"
#include "jsfs.h"
#include "jsdev.h"
#include "jsaudio.h"
#include "jseink.h"
#include "jspower.h"
#include "jsserial.h"
#define GP_W JSRT_PIC_W /* the direct picture's size, so screen.gameLowres() validates the same with any profile */
#define GP_H JSRT_PIC_H
#define PROF (jsrt_profile)
static int prof_live;                  /* init ran and stop has not */
/* The running app: its manifest's capabilities and paths. No APP.JSON means a legacy app (every capability,
   any path: v1 apps are trusted maker apps) unless jsrt_require_manifest; the picker is always unrestricted. */
int jsrt_require_manifest;
static struct app_manifest manifest;
static struct app_policy policy;
static int restricted;                 /* a manifest app: policy applies */
static uint32_t caps = APP_CAP_ALL;
static char app_id[16], entry_name[16] = "MAIN.JS";
static char start_who[24], start_why[96]; /* why the last app did not start (shown by the picker) */
static int orphan_mode;                /* the playing element was garbage collected: restore the page's mode next turn */

extern const char jsrt_prelude[];
extern const uint32_t jsrt_prelude_len;
extern const char jsrt_picker[];
extern const uint32_t jsrt_picker_len;
static int in_picker;
static char launch_dir[64];
static uint32_t app_heap_limit;
struct jsrt_stats jsrt_stats;
static JSRuntime *rt;
static JSContext *ctx;
static JSClassID el_class, style_class, ctx2d_class;
static JSValue native;                 /* N in prelude.js */
static double wake_at_ms = 1e300;      /* the earliest timer */
static int want_frame, exited, exit_code;
static uint64_t next_frame_ns, t0_ns;
#define FRAME_NS 16666667ull
/* One wrapper per element: the element, its object (not counted: the object
   owns the wrapper) and the objects it keeps alive. */
struct jel {struct dom_el *e; JSValue self, style, ctx2d; uint32_t fill;};
/* The game element (one at a time); what plays in it is the profile's. */
static struct dom_el *game_el;
static char game_dir[64];
static int game_started;
/* Screen modes: the page's (any the kernel takes; 640x480x32 at the start),
   and a game's own 320x200 at 8 bpp with its palette, when lowres is on (the
   default): the play core's picture goes straight to the screen. */
static uint32_t mode_w = PLAT_W, mode_h = PLAT_H, mode_d = 32, switch_us;
static int lowres = 1, lowres_on;
static uint32_t game_w = GP_W, game_h = GP_H; /* the game's mode: 320x200, or larger with the picture centered (384x216 is exactly 1080p / 5) */
static uint8_t shown_pal[768];         /* the palette last sent for the direct picture */
static int set_mode(uint32_t w, uint32_t h, uint32_t d) {
  if (plat_set_mode(w, h, d, &switch_us)) return -1;
  if (d != 8) {mode_w = w; mode_h = h; mode_d = d; dom_screen((int32_t)w, (int32_t)h, d);}
  return 0;
}

static void say(const char *s) {plat_log(s, (uint32_t)strlen(s));}
void jsrt_log(const char *s) {say(s);}
static void sayf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sayf(const char *fmt, ...) {
  char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); say(b);
}
/* Capability and path checks: a thrown error names the capability or the reason. */
static int need(JSContext *c, uint32_t any, const char *what) { /* any: one capability, or several of which one is enough */
  if (!restricted || (caps & any)) return 0;
  unsigned i = 0;
  while (!(any >> i & 1u)) i++;
  JS_ThrowPlainError(c, "%s: capability '%s' is not granted to this app", what, app_cap_name(i));
  return -1;
}
#define APP_CAP_ANY_READ (APP_CAP_FS_READ | APP_CAP_FS_READ_SHARED | APP_CAP_FS_WRITE_SAVES)
static int path_allowed(const char *path, int write, const char **why) {
  return !restricted || !app_path_check(&policy, path, write, why);
}
static int path_ok(JSContext *c, const char *path, int write, const char *what) {
  const char *why = "";
  if (path_allowed(path, write, &why)) return 0;
  JS_ThrowPlainError(c, "%s: path refused, %s: %s", what, why, path);
  return -1;
}
int32_t jsrt_fs_read(const char *path, uint8_t *buf, uint32_t cap) {return path_allowed(path, 0, 0) ? plat_read(path, buf, cap) : -1;}
int32_t jsrt_fs_list(const char *path, void (*fn)(void *, const char *, uint32_t, int, uint32_t), void *user) {return path_allowed(path, 0, 0) ? plat_list(path, fn, user) : -1;}
static double now_ms(void) {return (double)(plat_clock_ns() - t0_ns) / 1e6;}
static void report(void) {
  JSValue e = JS_GetException(ctx), f = JS_GetPropertyStr(ctx, native, "onError");
  if (JS_IsFunction(ctx, f)) JS_FreeValue(ctx, JS_Call(ctx, f, JS_UNDEFINED, 1, (JSValueConst *)&e));
  else {const char *s = JS_ToCString(ctx, e); sayf("JS error: %s", s ? s : "?"); JS_FreeCString(ctx, s);}
  JS_FreeValue(ctx, f); JS_FreeValue(ctx, e);
}
JSValue jsrt_call_native(const char *name, int argc, JSValueConst *argv);
static JSValue call_native(const char *name, int argc, JSValueConst *argv) {
  uint64_t t = plat_clock_ns();
  JSValue f = JS_GetPropertyStr(ctx, native, name), r = JS_UNDEFINED;
  if (JS_IsFunction(ctx, f)) {r = JS_Call(ctx, f, JS_UNDEFINED, argc, argv); if (JS_IsException(r)) report();}
  JS_FreeValue(ctx, f);
  jsrt_stats.js_ns += plat_clock_ns() - t;
  return r;
}

JSValue jsrt_call_native(const char *name, int argc, JSValueConst *argv) {return call_native(name, argc, argv);}
int jsrt_game_active(void) {return game_started || lowres_on;}
void jsrt_page_mode(uint32_t *w, uint32_t *h, uint32_t *d) {*w = mode_w; *h = mode_h; *d = mode_d;}
void jsrt_restore_page_mode(void) {set_mode(mode_w, mode_h, mode_d);}
/* Files: "/sd/..." paths; the reader the play core and pictures use. */
static uint8_t io[256 * 1024];
static int ends(const char *s, const char *x) {size_t a = strlen(s), b = strlen(x); if (a < b) return 0; for (size_t i = 0; i < b; i++) if ((s[a - b + i] | 32) != x[i]) return 0; return 1;}
/* dom.h's picture loader: a .GIF, or a game folder's title screen. */
static int load_picture(const char *src, uint8_t *px, uint8_t *pal) {
  if (!path_allowed(src, 0, 0)) return -1;
  if (ends(src, ".gif")) {
    int32_t n = plat_read(src, io, sizeof io);
    return n > 0 ? gp_gif(io, (uint32_t)n, pal, px) : -1;
  }
  if (game_started) return -1; /* a profile's picture reader may share state with the playing element */
  return PROF->picture ? PROF->picture(src, px, pal) : -1;
}

/* Element objects. */
static struct jel *jel_of(JSValueConst v) {return JS_GetOpaque(v, el_class);}
static struct dom_el *el_of(JSContext *c, JSValueConst v) {
  struct jel *j = jel_of(v);
  if (!j) {JS_ThrowTypeError(c, "not an element"); return 0;}
  return j->e;
}
static JSValue obj_of(struct dom_el *e) {return e ? JS_DupValue(ctx, ((struct jel *)e->js)->self) : JS_NULL;}
static void el_finalizer(JSRuntime *r, JSValueConst v) {
  struct jel *j = JS_GetOpaque(v, el_class);
  if (!j) return;
  struct dom_el *e = j->e;
  if (e == game_el) { /* a playing element nobody holds any more still gets end() */
    if (game_started) {game_started = 0; e->game = 0; if (PROF->end) PROF->end(); if (lowres_on) orphan_mode = 1;}
    game_el = 0;
  }
  while (e->first) {struct dom_el *c = e->first; dom_remove(c); JS_FreeValueRT(r, ((struct jel *)c->js)->self);}
  JS_FreeValueRT(r, j->style); JS_FreeValueRT(r, j->ctx2d);
  dom_delete(e); free(j);
}
static void el_mark(JSRuntime *r, JSValueConst v, JS_MarkFunc *mark) {
  struct jel *j = JS_GetOpaque(v, el_class);
  if (!j) return;
  for (struct dom_el *c = j->e->first; c; c = c->next) JS_MarkValue(r, ((struct jel *)c->js)->self, mark);
  JS_MarkValue(r, j->style, mark); JS_MarkValue(r, j->ctx2d, mark);
}
static JSValue new_element(JSContext *c, int tag) {
  struct dom_el *e = dom_new(tag);
  struct jel *j = calloc(1, sizeof *j);
  if (!e || !j) {free(j); if (e) dom_delete(e); return JS_ThrowOutOfMemory(c);}
  JSValue o = JS_NewObjectClass(c, (int)el_class);
  if (JS_IsException(o)) {free(j); dom_delete(e); return o;}
  j->e = e; j->self = o; j->style = j->ctx2d = JS_UNDEFINED; e->js = j;
  JS_SetOpaque(o, j);
  if (tag == DOM_CANVAS) dom_set_canvas_size(e, 300, 150);
  return o;
}
static JSValue js_create(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)this; (void)argc;
  if (need(c, APP_CAP_DISPLAY, "document.createElement")) return JS_EXCEPTION;
  const char *name = JS_ToCString(c, argv[0]);
  if (!name) return JS_EXCEPTION;
  char low[16]; size_t i = 0;
  for (; name[i] && i < sizeof low - 1; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
  low[i] = 0;
  int tag = dom_tag(low);
  JS_FreeCString(c, name);
  if (tag < 0) return JS_ThrowTypeError(c, "createElement: <%s> is not supported (div, text, span, p, h1, img, button, canvas, game)", low);
  return new_element(c, tag);
}
static int adopt(JSContext *c, struct dom_el *p, JSValueConst child, struct dom_el *before) {
  struct dom_el *k = el_of(c, child);
  if (!k) return -1;
  for (struct dom_el *a = p; a; a = a->parent) if (a == k) {JS_ThrowRangeError(c, "an element cannot hold itself"); return -1;}
  if (!k->parent) JS_DupValue(c, child); /* the parent keeps it alive */
  dom_insert_before(p, k, before);
  return 0;
}
static JSValue js_append(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; struct dom_el *p = el_of(c, this);
  if (!p || adopt(c, p, argv[0], 0)) return JS_EXCEPTION;
  return JS_DupValue(c, argv[0]);
}
static JSValue js_insert(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; struct dom_el *p = el_of(c, this), *b = JS_IsNull(argv[1]) || JS_IsUndefined(argv[1]) ? 0 : el_of(c, argv[1]);
  if (!p || (!b && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) || adopt(c, p, argv[0], b)) return JS_EXCEPTION;
  return JS_DupValue(c, argv[0]);
}
static void detach(JSContext *c, struct dom_el *k) {
  if (!k->parent) return;
  dom_remove(k);
  JS_FreeValue(c, ((struct jel *)k->js)->self);
}
static JSValue js_remove_child(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; struct dom_el *p = el_of(c, this), *k = el_of(c, argv[0]);
  if (!p || !k) return JS_EXCEPTION;
  if (k->parent != p) return JS_ThrowRangeError(c, "removeChild: not a child");
  JSValue r = JS_DupValue(c, argv[0]);
  detach(c, k);
  return r;
}
static JSValue js_remove(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; (void)argv; struct dom_el *k = el_of(c, this);
  if (!k) return JS_EXCEPTION;
  detach(c, k); return JS_UNDEFINED;
}
static void focus_to(struct dom_el *e);
static JSValue js_focus(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; (void)argv; struct dom_el *e = el_of(c, this);
  if (!e) return JS_EXCEPTION;
  if (e->focusable && dom_visible(e)) focus_to(e);
  return JS_UNDEFINED;
}
static JSValue js_blur(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; (void)argv; struct dom_el *e = el_of(c, this);
  if (!e) return JS_EXCEPTION;
  if (dom_focused == e) focus_to(0);
  return JS_UNDEFINED;
}
static JSValue js_rect(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; (void)argv; struct dom_el *e = el_of(c, this);
  if (!e) return JS_EXCEPTION;
  dom_layout();
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "x", JS_NewInt32(c, e->box.x)); JS_SetPropertyStr(c, o, "y", JS_NewInt32(c, e->box.y));
  JS_SetPropertyStr(c, o, "left", JS_NewInt32(c, e->box.x)); JS_SetPropertyStr(c, o, "top", JS_NewInt32(c, e->box.y));
  JS_SetPropertyStr(c, o, "width", JS_NewInt32(c, e->box.w)); JS_SetPropertyStr(c, o, "height", JS_NewInt32(c, e->box.h));
  return o;
}
enum {P_TAG, P_ID, P_CLASS, P_TEXT, P_SRC, P_WIDTH, P_HEIGHT, P_PARENT, P_FIRST, P_LAST, P_NEXT, P_PREV, P_CHILDREN, P_STYLE, P_TABINDEX, P_TICKS, P_PLAYING};
static JSValue el_get(JSContext *c, JSValueConst this, int magic) {
  struct jel *j = jel_of(this);
  if (!j) return JS_ThrowTypeError(c, "not an element");
  struct dom_el *e = j->e;
  switch (magic) {
  case P_TAG: {char t[12]; const char *n = dom_tag_name(e->tag); size_t i = 0; for (; n[i]; i++) t[i] = (char)(n[i] - 32); t[i] = 0; return JS_NewString(c, t);}
  case P_ID: return JS_NewString(c, e->id);
  case P_CLASS: return JS_NewString(c, e->cls);
  case P_TEXT: return JS_NewStringLen(c, e->text ? e->text : "", e->text_n);
  case P_SRC: return JS_NewString(c, e->tag == DOM_GAME ? (e == game_el ? game_dir : "") : "");
  case P_WIDTH: return JS_NewInt32(c, e->tag == DOM_CANVAS ? (int32_t)e->pw : e->box.w);
  case P_HEIGHT: return JS_NewInt32(c, e->tag == DOM_CANVAS ? (int32_t)e->ph : e->box.h);
  case P_PARENT: return e == dom_body ? JS_NULL : obj_of(e->parent);
  case P_FIRST: return obj_of(e->first);
  case P_LAST: return obj_of(e->last);
  case P_NEXT: return obj_of(e->next);
  case P_PREV: return obj_of(e->prev);
  case P_CHILDREN: {
    JSValue a = JS_NewArray(c); uint32_t i = 0;
    for (struct dom_el *k = e->first; k; k = k->next) JS_SetPropertyUint32(c, a, i++, obj_of(k));
    return a;
  }
  case P_STYLE:
    if (JS_IsUndefined(j->style)) {
      j->style = JS_NewObjectClass(c, (int)style_class);
      if (JS_IsException(j->style)) {j->style = JS_UNDEFINED; return JS_EXCEPTION;}
      JS_SetOpaque(j->style, j);
    }
    return JS_DupValue(c, j->style);
  case P_TABINDEX: return JS_NewInt32(c, e->focusable ? 0 : -1);
  case P_TICKS: return JS_NewInt32(c, e == game_el && PROF->ticks ? (int32_t)PROF->ticks() : 0);
  case P_PLAYING: return JS_NewBool(c, e == game_el && game_started);
  }
  return JS_UNDEFINED;
}
static int copy_str(JSContext *c, JSValueConst v, char *out, size_t cap) {
  size_t n; const char *s = JS_ToCStringLen(c, &n, v);
  if (!s) return -1;
  if (n != strlen(s)) {JS_FreeCString(c, s); JS_ThrowTypeError(c, "a NUL character is not allowed here"); return -1;}
  if (n >= cap) {JS_FreeCString(c, s); JS_ThrowRangeError(c, "too long (at most %u characters)", (unsigned)cap - 1); return -1;}
  memcpy(out, s, n + 1); JS_FreeCString(c, s);
  return 0;
}
static JSValue el_set(JSContext *c, JSValueConst this, JSValueConst v, int magic) {
  struct jel *j = jel_of(this);
  if (!j) return JS_ThrowTypeError(c, "not an element");
  struct dom_el *e = j->e; int32_t n;
  switch (magic) {
  case P_ID: if (copy_str(c, v, e->id, sizeof e->id)) return JS_EXCEPTION; break;
  case P_CLASS: if (copy_str(c, v, e->cls, sizeof e->cls)) return JS_EXCEPTION; dom_changed(e); break;
  case P_TEXT: {
    size_t len; const char *s = JS_ToCStringLen(c, &len, v);
    if (!s) return JS_EXCEPTION;
    int r = dom_set_text(e, s, (uint32_t)len); JS_FreeCString(c, s);
    if (r) return JS_ThrowOutOfMemory(c);
    break;
  }
  case P_SRC: {
    char src[64];
    if (copy_str(c, v, src, sizeof src)) return JS_EXCEPTION;
    if (e->tag == DOM_GAME) {
      if (e == game_el && game_started) return JS_ThrowRangeError(c, "the game is playing");
      snprintf(game_dir, sizeof game_dir, "%s", src); game_el = e;
    } else if (dom_set_src(e, src)) sayf("JS img: cannot show %s", src);
    break;
  }
  case P_WIDTH: case P_HEIGHT:
    if (JS_ToInt32(c, &n, v)) return JS_EXCEPTION;
    if (e->tag == DOM_CANVAS) {
      if (n < 1) n = 1;
      if (dom_set_canvas_size(e, magic == P_WIDTH ? (uint32_t)n : e->pw, magic == P_HEIGHT ? (uint32_t)n : e->ph)) return JS_ThrowRangeError(c, "canvas too big (640 x 480 at most)");
    } else {char b[16]; snprintf(b, sizeof b, "%d", (int)n); dom_set_style(e, magic == P_WIDTH ? "width" : "height", b);}
    break;
  case P_TABINDEX: if (JS_ToInt32(c, &n, v)) return JS_EXCEPTION; e->focusable = n >= 0; break;
  }
  return JS_UNDEFINED;
}
/* The game element: play() starts the folder in src; Esc or the game's end
   stops it and fires "ended". Keys go to the game while it is focused. */
static JSValue js_play(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; (void)argv; struct dom_el *e = el_of(c, this);
  if (!e) return JS_EXCEPTION;
  if (e->tag != DOM_GAME || e != game_el || !game_dir[0]) return JS_ThrowTypeError(c, "play(): a <game> with a src");
  if (!PROF->play) return JS_ThrowTypeError(c, "play(): this build has no game profile");
  if (need(c, APP_CAP_DISPLAY, "game.play") || path_ok(c, game_dir, 0, "game.play")) return JS_EXCEPTION;
  if (jsdev_leased()) return JS_ThrowPlainError(c, "EBUSY: the screen is leased; release() it before play()");
  char err[80] = ""; void *h = 0;
  if (PROF->play(game_dir, &h, err, sizeof err)) return JS_ThrowRangeError(c, "%s", err[0] ? err : "cannot play");
  if (lowres && !set_mode(game_w, game_h, 8)) {lowres_on = 1; memset(shown_pal, 0xff, sizeof shown_pal);}
  static char no_handle; /* a profile with no state still needs a non-NULL e->game: it gates drawing */
  e->game = h ? h : &no_handle; game_started = 1; jsrt_stats.game_ticks = 0; jsrt_stats.game_play_ns = 0; jsrt_stats.game_late_max_us = jsrt_stats.game_step_max_us = 0;
  focus_to(e); dom_changed(e);
  return JS_UNDEFINED;
}
static void stop_game(void) {
  if (!game_el) return;
  struct dom_el *e = game_el;
  game_started = 0; e->game = 0; dom_changed(e);
  if (PROF->end) PROF->end();
  if (lowres_on) {lowres_on = 0; set_mode(mode_w, mode_h, mode_d);} /* the page's mode again: it all repaints */
  JSValue args[2] = {JS_DupValue(ctx, ((struct jel *)e->js)->self), JS_NewString(ctx, "ended")};
  JS_FreeValue(ctx, call_native("onEvent", 2, args));
  JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
}
static JSValue js_stop(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; (void)argv; struct dom_el *e = el_of(c, this);
  if (!e) return JS_EXCEPTION;
  if (e == game_el && game_started) stop_game();
  return JS_UNDEFINED;
}
/* game.press(linuxKeyCode, down): a key for the game, as if pressed (demos, bots, benchmarks). */
static JSValue js_press(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  struct dom_el *e = el_of(c, this); int32_t code, down = 1;
  if (!e || JS_ToInt32(c, &code, argv[0]) || (argc > 1 && JS_ToInt32(c, &down, argv[1]))) return JS_EXCEPTION;
  if (e == game_el && game_started && PROF->key && PROF->key((uint16_t)code, down ? 1 : 0) & JSRT_END) stop_game();
  return JS_UNDEFINED;
}
static JSValue js_get_context(JSContext *c, JSValueConst this, int argc, JSValueConst *argv);
static const JSCFunctionListEntry el_proto[] = {
  JS_CGETSET_MAGIC_DEF("tagName", el_get, 0, P_TAG), JS_CGETSET_MAGIC_DEF("id", el_get, el_set, P_ID),
  JS_CGETSET_MAGIC_DEF("className", el_get, el_set, P_CLASS), JS_CGETSET_MAGIC_DEF("textContent", el_get, el_set, P_TEXT),
  JS_CGETSET_MAGIC_DEF("src", el_get, el_set, P_SRC), JS_CGETSET_MAGIC_DEF("width", el_get, el_set, P_WIDTH),
  JS_CGETSET_MAGIC_DEF("height", el_get, el_set, P_HEIGHT), JS_CGETSET_MAGIC_DEF("parentNode", el_get, 0, P_PARENT),
  JS_CGETSET_MAGIC_DEF("parentElement", el_get, 0, P_PARENT), JS_CGETSET_MAGIC_DEF("firstChild", el_get, 0, P_FIRST),
  JS_CGETSET_MAGIC_DEF("lastChild", el_get, 0, P_LAST), JS_CGETSET_MAGIC_DEF("nextSibling", el_get, 0, P_NEXT),
  JS_CGETSET_MAGIC_DEF("previousSibling", el_get, 0, P_PREV), JS_CGETSET_MAGIC_DEF("children", el_get, 0, P_CHILDREN),
  JS_CGETSET_MAGIC_DEF("style", el_get, 0, P_STYLE), JS_CGETSET_MAGIC_DEF("tabIndex", el_get, el_set, P_TABINDEX),
  JS_CGETSET_MAGIC_DEF("ticks", el_get, 0, P_TICKS), JS_CGETSET_MAGIC_DEF("playing", el_get, 0, P_PLAYING),
  JS_CFUNC_DEF("appendChild", 1, js_append), JS_CFUNC_DEF("insertBefore", 2, js_insert),
  JS_CFUNC_DEF("removeChild", 1, js_remove_child), JS_CFUNC_DEF("remove", 0, js_remove),
  JS_CFUNC_DEF("focus", 0, js_focus), JS_CFUNC_DEF("blur", 0, js_blur),
  JS_CFUNC_DEF("getBoundingClientRect", 0, js_rect), JS_CFUNC_DEF("getContext", 1, js_get_context),
  JS_CFUNC_DEF("play", 0, js_play), JS_CFUNC_DEF("stop", 0, js_stop), JS_CFUNC_DEF("press", 2, js_press),
};

/* element.style: one accessor per supported property (dom.c's CSS subset). */
static const char *const style_names[] = {"left", "top", "width", "height", "color", "background", "backgroundColor", "display",
  "flexDirection", "gap", "padding", "border", "borderWidth", "borderColor", "outlineColor", "fontSize", "textAlign", "position"};
#define NSTYLE (sizeof style_names / sizeof style_names[0])
static JSValue style_get(JSContext *c, JSValueConst this, int magic) {
  struct jel *j = JS_GetOpaque(this, style_class);
  if (!j) return JS_ThrowTypeError(c, "not a style");
  char b[48];
  return dom_get_style(j->e, style_names[magic], b, sizeof b) < 0 ? JS_UNDEFINED : JS_NewString(c, b);
}
static JSValue style_set(JSContext *c, JSValueConst this, JSValueConst v, int magic) {
  struct jel *j = JS_GetOpaque(this, style_class);
  if (!j) return JS_ThrowTypeError(c, "not a style");
  char b[48];
  if (JS_IsNumber(v)) {double d; JS_ToFloat64(c, &d, v); snprintf(b, sizeof b, "%dpx", (int)d);}
  else if (copy_str(c, v, b, sizeof b)) return JS_EXCEPTION;
  if (dom_set_style(j->e, style_names[magic], b)) sayf("JS style.%s = '%s' is outside the CSS subset; ignored", style_names[magic], b);
  return JS_UNDEFINED;
}
static JSCFunctionListEntry style_proto[NSTYLE];
static void style_mark(JSRuntime *r, JSValueConst v, JS_MarkFunc *mark) {
  struct jel *j = JS_GetOpaque(v, style_class);
  if (j) JS_MarkValue(r, j->self, mark);
}
static void style_finalizer(JSRuntime *r, JSValueConst v) {(void)r; (void)v;}

/* canvas.getContext('2d'): fillStyle, fillRect, clearRect, fillText (the 5x9
   font; font "18px" doubles it), drawImage(img or canvas, x, y[, w, h]). */
static JSValue js_get_context(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  (void)argc; (void)argv; struct jel *j = jel_of(this);
  if (!j || j->e->tag != DOM_CANVAS) return JS_NULL;
  if (JS_IsUndefined(j->ctx2d)) {
    j->ctx2d = JS_NewObjectClass(c, (int)ctx2d_class);
    if (JS_IsException(j->ctx2d)) {j->ctx2d = JS_UNDEFINED; return JS_EXCEPTION;}
    JS_SetOpaque(j->ctx2d, j); j->fill = 0;
  }
  return JS_DupValue(c, j->ctx2d);
}
static struct jel *cx_of(JSContext *c, JSValueConst v) {
  struct jel *j = JS_GetOpaque(v, ctx2d_class);
  if (!j) JS_ThrowTypeError(c, "not a 2d context");
  return j;
}
static int ints(JSContext *c, int argc, JSValueConst *argv, int from, int n, int32_t *out) {
  for (int i = 0; i < n; i++) {double d = 0; if (from + i < argc && JS_ToFloat64(c, &d, argv[from + i])) return -1; out[i] = (int32_t)d;}
  return 0;
}
static JSValue cx_fill_rect(JSContext *c, JSValueConst this, int argc, JSValueConst *argv, int clear) {
  struct jel *j = cx_of(c, this); int32_t v[4];
  if (!j || ints(c, argc, argv, 0, 4, v)) return JS_EXCEPTION;
  dom_canvas_fill(j->e, v[0], v[1], v[2], v[3], clear ? 0 : j->fill);
  return JS_UNDEFINED;
}
static int font_scale = 1;
static JSValue cx_fill_text(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  struct jel *j = cx_of(c, this); int32_t v[2]; size_t n;
  if (!j || ints(c, argc, argv, 1, 2, v)) return JS_EXCEPTION;
  const char *s = JS_ToCStringLen(c, &n, argv[0]);
  if (!s) return JS_EXCEPTION;
  dom_canvas_text(j->e, v[0], v[1], j->fill, font_scale, s, (uint32_t)n);
  JS_FreeCString(c, s);
  return JS_UNDEFINED;
}
static JSValue cx_draw_image(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
  struct jel *j = cx_of(c, this), *src = argc ? jel_of(argv[0]) : 0; int32_t v[4];
  if (!j || !src || ints(c, argc, argv, 1, 4, v)) return j && !src ? JS_ThrowTypeError(c, "drawImage: an img or canvas") : JS_EXCEPTION;
  if (argc < 5) {v[2] = src->e->tag == DOM_CANVAS ? (int32_t)src->e->pw : 320; v[3] = src->e->tag == DOM_CANVAS ? (int32_t)src->e->ph : 200;}
  dom_canvas_image(j->e, src->e, v[0], v[1], v[2], v[3]);
  return JS_UNDEFINED;
}
enum {C_FILL, C_FONT};
static JSValue cx_get(JSContext *c, JSValueConst this, int magic) {
  struct jel *j = cx_of(c, this); char b[16];
  if (!j) return JS_EXCEPTION;
  if (magic == C_FILL) {snprintf(b, sizeof b, "#%06x", (unsigned)j->fill); return JS_NewString(c, b);}
  snprintf(b, sizeof b, "%dpx", font_scale * 9); return JS_NewString(c, b);
}
static JSValue cx_set(JSContext *c, JSValueConst this, JSValueConst v, int magic) {
  struct jel *j = cx_of(c, this); char b[48];
  if (!j || copy_str(c, v, b, sizeof b)) return JS_EXCEPTION;
  if (magic == C_FILL) {uint32_t rgb; if (!dom_parse_color(b, &rgb)) j->fill = rgb;}
  else {int px = atoi(b); font_scale = px >= 18 ? px / 9 : 1; if (font_scale > 4) font_scale = 4;}
  return JS_UNDEFINED;
}
static JSValue cx_fill(JSContext *c, JSValueConst t, int n, JSValueConst *a) {return cx_fill_rect(c, t, n, a, 0);}
static JSValue cx_clear(JSContext *c, JSValueConst t, int n, JSValueConst *a) {return cx_fill_rect(c, t, n, a, 1);}
static const JSCFunctionListEntry ctx2d_proto[] = {
  JS_CGETSET_MAGIC_DEF("fillStyle", cx_get, cx_set, C_FILL), JS_CGETSET_MAGIC_DEF("font", cx_get, cx_set, C_FONT),
  JS_CFUNC_DEF("fillRect", 4, cx_fill), JS_CFUNC_DEF("clearRect", 4, cx_clear),
  JS_CFUNC_DEF("fillText", 3, cx_fill_text), JS_CFUNC_DEF("drawImage", 5, cx_draw_image),
};
static void ctx2d_mark(JSRuntime *r, JSValueConst v, JS_MarkFunc *mark) {style_mark(r, v, mark);}

/* Finding elements: "#id", ".class", or a tag name. */
static int matches(const struct dom_el *e, const char *sel) {
  if (sel[0] == '#') return !strcmp(e->id, sel + 1);
  if (sel[0] == '.') {
    size_t n = strlen(sel + 1);
    for (const char *p = e->cls; *p;) {
      while (*p == ' ') p++;
      const char *q = p; while (*q && *q != ' ') q++;
      if ((size_t)(q - p) == n && n && !memcmp(p, sel + 1, n)) return 1;
      p = q;
    }
    return 0;
  }
  return dom_tag(sel) == e->tag;
}
static void collect(JSContext *c, struct dom_el *e, const char *sel, JSValue all, uint32_t *n, struct dom_el **one) {
  for (struct dom_el *k = e->first; k; k = k->next) {
    if (one && *one) return;
    if (matches(k, sel)) {if (one) {*one = k; return;} JS_SetPropertyUint32(c, all, (*n)++, obj_of(k));}
    collect(c, k, sel, all, n, one);
  }
}
static JSValue find(JSContext *c, JSValueConst sel_v, int many, int by_id) {
  char sel[48];
  if (by_id) {sel[0] = '#'; if (copy_str(c, sel_v, sel + 1, sizeof sel - 1)) return JS_EXCEPTION;}
  else if (copy_str(c, sel_v, sel, sizeof sel)) return JS_EXCEPTION;
  struct dom_el *one = 0; uint32_t n = 0;
  if (many) {JSValue a = JS_NewArray(c); collect(c, dom_body, sel, a, &n, 0); return a;}
  if (matches(dom_body, sel)) one = dom_body; else collect(c, dom_body, sel, JS_UNDEFINED, &n, &one);
  return obj_of(one);
}
static JSValue js_by_id(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; return find(c, a[0], 0, 1);}
static JSValue js_query(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; return find(c, a[0], 0, 0);}
static JSValue js_query_all(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; return find(c, a[0], 1, 0);}

/* The rest of N. */
static JSValue js_focused(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)c; (void)t; (void)n; (void)a; return obj_of(dom_focused);}
static JSValue js_now(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; (void)a; return JS_NewFloat64(c, now_ms());}
static JSValue js_wake_at(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; double d; if (JS_ToFloat64(c, &d, a[0])) return JS_EXCEPTION; wake_at_ms = d; return JS_UNDEFINED;}
static JSValue js_want_frame(JSContext *c, JSValueConst t, int n, JSValueConst *a) {(void)t; (void)n; (void)a; if (need(c, APP_CAP_DISPLAY, "requestAnimationFrame")) return JS_EXCEPTION; want_frame = 1; return JS_UNDEFINED;}
static JSValue js_log(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; size_t len; const char *s = JS_ToCStringLen(c, &len, a[0]);
  if (!s) return JS_EXCEPTION;
  for (size_t at = 0; at < len;) { /* the kernel's log lines hold 120 characters: one line each */
    size_t e = at; while (e < len && s[e] != '\n' && e - at < 116) e++;
    char line[124] = "JS "; memcpy(line + 3, s + at, e - at); plat_log(line, (uint32_t)(e - at + 3));
    at = e < len && s[e] == '\n' ? e + 1 : e;
  }
  JS_FreeCString(c, s);
  return JS_UNDEFINED;
}
static JSValue js_read_file(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char path[96];
  if (need(c, APP_CAP_ANY_READ, "fs.readFile") || copy_str(c, a[0], path, sizeof path) || path_ok(c, path, 0, "fs.readFile")) return JS_EXCEPTION;
  int32_t got = plat_read(path, io, sizeof io);
  if (got < 0) return JS_ThrowReferenceError(c, "fs.readFile: no file %s", path);
  return JS_NewStringLen(c, (const char *)io, (size_t)got);
}
struct lister {JSContext *c; JSValue a; uint32_t n;};
static void list_one(void *user, const char *name, uint32_t n, int dir, uint32_t size) {
  (void)dir; (void)size; struct lister *l = user;
  JS_SetPropertyUint32(l->c, l->a, l->n++, JS_NewStringLen(l->c, name, n));
}
static JSValue js_readdir(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char path[96];
  if (need(c, APP_CAP_ANY_READ, "fs.readdir") || copy_str(c, a[0], path, sizeof path) || path_ok(c, path, 0, "fs.readdir")) return JS_EXCEPTION;
  struct lister l = {c, JS_NewArray(c), 0};
  if (plat_list(path, list_one, &l) < 0) {JS_FreeValue(c, l.a); return JS_ThrowReferenceError(c, "fs.readdir: no folder %s", path);}
  return l.a;
}
static JSValue js_exit(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; int32_t code = 0;
  if (n && JS_ToInt32(c, &code, a[0])) return JS_EXCEPTION;
  if (!exited) {exited = 1; exit_code = code;}
  return JS_UNDEFINED;
}
static JSValue js_exiting(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; return JS_NewBool(c, exited);
}
/* Only the built-in picker can request another app; no kernel exec ABI. */
static JSValue js_launch(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char name[9];
  if (!in_picker) return JS_ThrowTypeError(c, "launch: picker only");
  if (copy_str(c, a[0], name, sizeof name)) return JS_EXCEPTION;
  if (!name[0]) return JS_ThrowRangeError(c, "empty app name");
  for (const char *p = name; *p; p++)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
      return JS_ThrowRangeError(c, "not a DOS app folder");
  if (!exited) {snprintf(launch_dir, sizeof launch_dir, "/sd/OS/APPS/%s", name); exited = 1; exit_code = 0;}
  return JS_UNDEFINED;
}
static JSValue js_stats(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "startUs", JS_NewFloat64(c, (double)jsrt_stats.start_ns / 1e3));
  JS_SetPropertyStr(c, o, "jsUs", JS_NewFloat64(c, (double)jsrt_stats.js_ns / 1e3));
  JS_SetPropertyStr(c, o, "renderUs", JS_NewFloat64(c, (double)jsrt_stats.render_ns / 1e3));
  JS_SetPropertyStr(c, o, "presentUs", JS_NewFloat64(c, (double)jsrt_stats.present_ns / 1e3));
  JS_SetPropertyStr(c, o, "frames", JS_NewUint32(c, jsrt_stats.frames));
  JS_SetPropertyStr(c, o, "presents", JS_NewUint32(c, jsrt_stats.presents));
  JS_SetPropertyStr(c, o, "layouts", JS_NewUint32(c, dom_stats.layouts));
  JS_SetPropertyStr(c, o, "paints", JS_NewUint32(c, dom_stats.paints));
  JS_SetPropertyStr(c, o, "rects", JS_NewUint32(c, dom_stats.rects));
  JS_SetPropertyStr(c, o, "pixels", JS_NewUint32(c, dom_stats.pixels));
  JS_SetPropertyStr(c, o, "gameTicks", JS_NewUint32(c, jsrt_stats.game_ticks));
  JS_SetPropertyStr(c, o, "gamePlayMs", JS_NewFloat64(c, (double)jsrt_stats.game_play_ns / 1e6));
  JS_SetPropertyStr(c, o, "inputMaxPerStep", JS_NewUint32(c, jsrt_stats.input_max));
  JS_SetPropertyStr(c, o, "gameLateMaxUs", JS_NewUint32(c, jsrt_stats.game_late_max_us));
  JS_SetPropertyStr(c, o, "gameStepMaxUs", JS_NewUint32(c, jsrt_stats.game_step_max_us));
  return o;
}
static JSValue js_memory(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; JSMemoryUsage u; JS_ComputeMemoryUsage(rt, &u);
  JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "mallocSize", JS_NewInt64(c, u.malloc_size));
  JS_SetPropertyStr(c, o, "mallocCount", JS_NewInt64(c, u.malloc_count));
  JS_SetPropertyStr(c, o, "memoryUsed", JS_NewInt64(c, u.memory_used_size));
  JS_SetPropertyStr(c, o, "objects", JS_NewInt64(c, u.obj_count));
  return o;
}
static JSValue js_set_mode(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; int32_t w, h, d = 32;
  if (need(c, APP_CAP_DISPLAY, "screen.setMode")) return JS_EXCEPTION;
  if (JS_ToInt32(c, &w, a[0]) || JS_ToInt32(c, &h, a[1]) || (n > 2 && JS_ToInt32(c, &d, a[2]))) return JS_EXCEPTION;
  if (d != 32 && d != 16) return JS_ThrowRangeError(c, "screen.setMode: depth 32 or 16 for pages (8 is the game's)");
  if (jsdev_leased()) return JS_ThrowPlainError(c, "EBUSY: the screen is leased; release() it before setMode");
  if (lowres_on || set_mode((uint32_t)w, (uint32_t)h, (uint32_t)d)) return JS_ThrowRangeError(c, "screen.setMode(%d, %d, %d) refused", (int)w, (int)h, (int)d);
  sayf("JS screen %dx%dx%d in %u us", (int)w, (int)h, (int)d, (unsigned)switch_us);
  return JS_NewUint32(c, switch_us);
}
static JSValue js_screen(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "width", JS_NewUint32(c, mode_w)); JS_SetPropertyStr(c, o, "height", JS_NewUint32(c, mode_h));
  JS_SetPropertyStr(c, o, "depth", JS_NewUint32(c, mode_d)); JS_SetPropertyStr(c, o, "switchUs", JS_NewUint32(c, switch_us));
  return o;
}
static JSValue js_lowres(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; int32_t w = GP_W, h = GP_H;
  if (need(c, APP_CAP_DISPLAY, "screen.gameLowres")) return JS_EXCEPTION;
  lowres = JS_ToBool(c, a[0]);
  if (n > 2 && (JS_ToInt32(c, &w, a[1]) || JS_ToInt32(c, &h, a[2]))) return JS_EXCEPTION;
  if (w < (int32_t)GP_W || h < (int32_t)GP_H || w % 4 || w * h > 640 * 480) return JS_ThrowRangeError(c, "screen.gameLowres: at least 320x200, width a multiple of 4");
  game_w = (uint32_t)w; game_h = (uint32_t)h;
  return JS_UNDEFINED;
}
/* The file API (fs.open, fs.stat, writers) is jsfs.c; it asks for the checks through these. */
int jsrt_need(JSContext *c, uint32_t any_cap, const char *what) {return need(c, any_cap, what);}
int jsrt_path_check(const char *path, int write, const char **why) {
  static const char *none; if (!why) why = &none;
  return !restricted || !app_path_check(&policy, path, write, why) ? 0 : -1;
}
int jsrt_copy_str(JSContext *c, JSValueConst v, char *out, size_t cap) {return copy_str(c, v, out, cap);}
uint8_t *jsrt_io(size_t *size) {*size = sizeof io; return io;}
/* sandbox.*: what this runtime and app are. */
static JSValue js_caps(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; JSValue arr = JS_NewArray(c); uint32_t k = 0;
  for (unsigned i = 0; app_cap_name(i); i++) if (!restricted || (caps & (1u << i))) JS_SetPropertyUint32(c, arr, k++, JS_NewString(c, app_cap_name(i)));
  return arr;
}
static JSValue js_app(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "id", JS_NewString(c, app_id)); JS_SetPropertyStr(c, o, "legacy", JS_NewBool(c, !restricted));
  JS_SetPropertyStr(c, o, "name", JS_NewString(c, restricted ? manifest.name : app_id));
  JS_SetPropertyStr(c, o, "version", JS_NewString(c, restricted ? manifest.version : "0"));
  JS_SetPropertyStr(c, o, "api", JS_NewUint32(c, APP_API_VERSION)); JS_SetPropertyStr(c, o, "runtime", JS_NewString(c, JSRT_VERSION));
  return o;
}
static JSValue js_require(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; char name[24];
  if (copy_str(c, a[0], name, sizeof name)) return JS_EXCEPTION;
  uint32_t bit = app_cap_bit(name);
  if (!bit) return JS_ThrowTypeError(c, "sandbox.require: unknown capability '%s'", name);
  return need(c, bit, "sandbox.require") ? JS_EXCEPTION : JS_UNDEFINED;
}
static JSValue js_start_error(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a;
  if (!start_who[0]) return JS_NULL;
  JSValue arr = JS_NewArray(c);
  JS_SetPropertyUint32(c, arr, 0, JS_NewString(c, start_who)); JS_SetPropertyUint32(c, arr, 1, JS_NewString(c, start_why));
  return arr;
}

/* ---- apps.*: list, launch and exit between JS apps (gap G5). The kernel has no exec, so the runtime switches the app inside
   JS.ELF: the current app is stopped (every handle released), the target started, and a small stack remembers who launched
   whom. The state below outlives an app; jsapp_step moves it. ---- */
#define LAUNCH_DEPTH 4u
#define ARGS_MAX 200u
#define LIST_MAX 32u
enum {PEND_NONE, PEND_LAUNCH, PEND_EXIT};
static struct {char id[9]; char args[ARGS_MAX + 1];} lstack[LAUNCH_DEPTH]; /* the apps waiting for their callee to exit */
static unsigned ldepth;
static int pending;                    /* what the running app asked for when it stopped */
static char pend_id[9], pend_args[ARGS_MAX + 1], cur_args[ARGS_MAX + 1];
static char ret_id[9]; static int ret_code, ret_set;              /* who returned to this app, and with what */
static char fail_id[9], fail_why[96];                              /* a launch that did not start, told to the caller */
static char autorun_dir[48];
static JSValue js_apps_launch(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; char id[9]; 
  if (need(c, APP_CAP_APPS_LAUNCH, "apps.launch")) return JS_EXCEPTION;
  if (copy_str(c, a[0], id, sizeof id)) return JS_EXCEPTION;
  if (!id[0]) return JS_ThrowRangeError(c, "apps.launch: empty app id");
  for (const char *p = id; *p; p++)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return JS_ThrowRangeError(c, "apps.launch: not an app id (a folder name in /OS/APPS)");
  if (ldepth >= LAUNCH_DEPTH) return JS_ThrowRangeError(c, "apps.launch: apps are launched %u deep already (apps.exit() first)", LAUNCH_DEPTH);
  if (pending) return JS_UNDEFINED; /* one request: the first one wins until the app stops */
  char text[ARGS_MAX + 1] = "";
  if (n > 1 && !JS_IsUndefined(a[1]) && copy_str(c, a[1], text, sizeof text)) return JS_EXCEPTION;
  snprintf(pend_id, sizeof pend_id, "%s", id); snprintf(pend_args, sizeof pend_args, "%s", text);
  pending = PEND_LAUNCH; exit_code = 0; exited = 1;
  return JS_UNDEFINED;
}
static JSValue js_apps_exit(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; int32_t code = 0;
  if (need(c, APP_CAP_APPS_LAUNCH, "apps.exit")) return JS_EXCEPTION;
  if (n && JS_ToInt32(c, &code, a[0])) return JS_EXCEPTION;
  if (!pending) {pending = PEND_EXIT; exit_code = code; exited = 1;}
  return JS_UNDEFINED;
}
/* One app's metadata for apps.list(): the same validator as the launch, no access to the app's files. */
static JSValue app_info(JSContext *c, const char *id) {
  char dir[48], path[64], err[96] = "", reason[96] = "";
  snprintf(dir, sizeof dir, "/sd/OS/APPS/%s", id);
  JSValue o = JS_NewObject(c);
  struct app_manifest m; int have = 0, ok = 1;
  snprintf(path, sizeof path, "%s/APP.JSON", dir);
  int32_t n = plat_read(path, io, APP_MANIFEST_MAX + 1);
  if (n < 0) {
    snprintf(path, sizeof path, "%s/MAIN.JS", dir);
    if (plat_read(path, io, 1) < 0) {JS_FreeValue(c, o); return JS_UNDEFINED;} /* not an app folder */
    if (jsrt_require_manifest) {ok = 0; snprintf(reason, sizeof reason, "APP.JSON is missing");}
    memset(&m, 0, sizeof m); strcpy(m.version, "0"); strcpy(m.entry, "MAIN.JS"); m.caps = APP_CAP_ALL; m.api = 0;
  } else if (app_manifest_parse((const char *)io, (uint32_t)n, &m, err, sizeof err)) {
    ok = 0; snprintf(reason, sizeof reason, "%s", err);
  } else {
    have = 1;
    if (m.api > APP_API_VERSION) {ok = 0; snprintf(reason, sizeof reason, "needs runtime API %u, this is %u", (unsigned)m.api, APP_API_VERSION);}
  }
  if (ok) {
    snprintf(path, sizeof path, "%s/%s", dir, m.entry);
    if (plat_read(path, io, 1) < 0) {ok = 0; snprintf(reason, sizeof reason, "%s is not in the app folder", m.entry);}
  }
  JS_SetPropertyStr(c, o, "id", JS_NewString(c, id));
  JS_SetPropertyStr(c, o, "name", JS_NewString(c, have ? m.name : id));
  JS_SetPropertyStr(c, o, "version", JS_NewString(c, m.version));
  JS_SetPropertyStr(c, o, "api", JS_NewUint32(c, m.api));
  JSValue arr = JS_NewArray(c); uint32_t k = 0;
  if (have) for (unsigned i = 0; app_cap_name(i); i++) if (m.caps & (1u << i)) JS_SetPropertyUint32(c, arr, k++, JS_NewString(c, app_cap_name(i)));
  JS_SetPropertyStr(c, o, "capabilities", arr);
  JS_SetPropertyStr(c, o, "ok", JS_NewBool(c, ok));
  JS_SetPropertyStr(c, o, "reason", JS_NewString(c, reason));
  return o;
}
struct idlist {char id[LIST_MAX][9]; unsigned n;};
static void collect_id(void *user, const char *name, uint32_t n, int dir, uint32_t size) {
  (void)size; struct idlist *l = user; char id[9];
  if (!dir || n < 1 || n > 8 || l->n >= LIST_MAX) return;
  memcpy(id, name, n); id[n] = 0;
  for (const char *p = id; *p; p++) if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return;
  strcpy(l->id[l->n++], id);
}
static JSValue js_apps_list(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a;
  if (need(c, APP_CAP_APPS_LAUNCH, "apps.list")) return JS_EXCEPTION;
  static struct idlist ids; ids.n = 0;
  plat_list("/sd/OS/APPS", collect_id, &ids);
  JSValue arr = JS_NewArray(c); uint32_t k = 0;
  for (unsigned i = 0; i < ids.n; i++) {
    JSValue o = app_info(c, ids.id[i]);
    if (!JS_IsUndefined(o)) JS_SetPropertyUint32(c, arr, k++, o);
  }
  return arr;
}
/* What happened before this app started: its launch arguments (JSON text or ""), who returned to it and with what code, and a
   launch of its own that did not start. */
static JSValue js_launch_info(JSContext *c, JSValueConst t, int n, JSValueConst *a) {
  (void)t; (void)n; (void)a; JSValue o = JS_NewObject(c);
  JS_SetPropertyStr(c, o, "args", JS_NewString(c, cur_args));
  JS_SetPropertyStr(c, o, "depth", JS_NewUint32(c, ldepth));
  if (ret_set) {JSValue r = JS_NewObject(c); JS_SetPropertyStr(c, r, "id", JS_NewString(c, ret_id)); JS_SetPropertyStr(c, r, "code", JS_NewInt32(c, ret_code)); JS_SetPropertyStr(c, o, "returned", r);}
  if (fail_id[0]) {JSValue r = JS_NewObject(c); JS_SetPropertyStr(c, r, "id", JS_NewString(c, fail_id)); JS_SetPropertyStr(c, r, "reason", JS_NewString(c, fail_why)); JS_SetPropertyStr(c, o, "failed", r);}
  return o;
}
static const JSCFunctionListEntry native_fns[] = {
  JS_CFUNC_DEF("caps", 0, js_caps), JS_CFUNC_DEF("app", 0, js_app), JS_CFUNC_DEF("require", 1, js_require),
  JS_CFUNC_DEF("startError", 0, js_start_error),
  JS_CFUNC_DEF("setMode", 3, js_set_mode), JS_CFUNC_DEF("screen", 0, js_screen), JS_CFUNC_DEF("gameLowres", 3, js_lowres),
  JS_CFUNC_DEF("createElement", 1, js_create), JS_CFUNC_DEF("getElementById", 1, js_by_id),
  JS_CFUNC_DEF("querySelector", 1, js_query), JS_CFUNC_DEF("querySelectorAll", 1, js_query_all),
  JS_CFUNC_DEF("focused", 0, js_focused), JS_CFUNC_DEF("now", 0, js_now), JS_CFUNC_DEF("wakeAt", 1, js_wake_at),
  JS_CFUNC_DEF("wantFrame", 0, js_want_frame), JS_CFUNC_DEF("log", 1, js_log), JS_CFUNC_DEF("readFile", 1, js_read_file),
  JS_CFUNC_DEF("readdir", 1, js_readdir), JS_CFUNC_DEF("exit", 1, js_exit), JS_CFUNC_DEF("stats", 0, js_stats),
  JS_CFUNC_DEF("memory", 0, js_memory), JS_CFUNC_DEF("exiting", 0, js_exiting),
  JS_CFUNC_DEF("launch", 1, js_launch),
  JS_CFUNC_DEF("appsList", 0, js_apps_list), JS_CFUNC_DEF("appsLaunch", 2, js_apps_launch), JS_CFUNC_DEF("appsExit", 1, js_apps_exit),
  JS_CFUNC_DEF("launchInfo", 0, js_launch_info),
};

/* Focus changes fire blur and focus. */
static void fire(struct dom_el *e, const char *type) {
  if (!e || !e->js) return;
  JSValue args[2] = {JS_DupValue(ctx, ((struct jel *)e->js)->self), JS_NewString(ctx, type)};
  JS_FreeValue(ctx, call_native("onEvent", 2, args));
  JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
}
static void focus_to(struct dom_el *e) {
  struct dom_el *was = dom_focused;
  if (was == e) return;
  dom_focus(e);
  if (was) fire(was, "blur");
  if (e) fire(e, "focus");
}
/* Keys: Linux codes to DOM key and code names. */
static const char *key_name(uint16_t k, const char **code) {
  static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
  static char key[2], cbuf[8];
  switch (k) {
  case 103: *code = "ArrowUp"; return "ArrowUp";
  case 108: *code = "ArrowDown"; return "ArrowDown";
  case 105: *code = "ArrowLeft"; return "ArrowLeft";
  case 106: *code = "ArrowRight"; return "ArrowRight";
  case 28: case 96: *code = "Enter"; return "Enter";
  case 1: *code = "Escape"; return "Escape";
  case 57: *code = "Space"; return " ";
  case 14: *code = "Backspace"; return "Backspace";
  case 15: *code = "Tab"; return "Tab";
  }
  char ch = 0;
  if (k >= 16 && k <= 25) ch = row1[k - 16]; else if (k >= 30 && k <= 38) ch = row2[k - 30]; else if (k >= 44 && k <= 50) ch = row3[k - 44];
  if (ch) {key[0] = ch; key[1] = 0; snprintf(cbuf, sizeof cbuf, "Key%c", ch - 32); *code = cbuf; return key;}
  if (k >= 2 && k <= 11) {key[0] = (char)(k == 11 ? '0' : '1' + k - 2); key[1] = 0; snprintf(cbuf, sizeof cbuf, "Digit%c", key[0]); *code = cbuf; return key;}
  *code = "Unidentified"; return "Unidentified";
}
static void key_event(uint16_t code, int32_t value) {
  if (!(caps & APP_CAP_INPUT) && restricted) return; /* no input capability: the app sees no keys (F10 is the host's) */
  if (game_started && dom_focused == game_el) { /* the game has the keys */
    if (PROF->key && PROF->key(code, value) & JSRT_END) stop_game();
    return;
  }
  const char *cname, *k = key_name(code, &cname);
  JSValue args[4] = {JS_NewString(ctx, value ? "keydown" : "keyup"), JS_NewString(ctx, k), JS_NewString(ctx, cname), JS_NewBool(ctx, value == 2)};
  JSValue r = call_native("onKey", 4, args);
  int go_on = JS_ToBool(ctx, r);
  JS_FreeValue(ctx, r);
  for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
  if (exited || !value || !go_on) return; /* keyup, exit, or preventDefault() */
  int dir = code == 103 ? 0 : code == 108 ? 1 : code == 105 ? 2 : code == 106 ? 3 : -1; /* the default actions */
  struct dom_el *was = dom_focused;
  if (dir >= 0 && dom_focus_move(dir)) {fire(was, "blur"); fire(dom_focused, "focus");}
  else if ((code == 28 || code == 57 || code == 96) && dom_focused && value == 1) fire(dom_focused, "click");
}

int jsrt_eval(const char *src, uint32_t n, const char *name) {
  uint64_t t = plat_clock_ns();
  JSValue r = JS_Eval(ctx, src, n, name, JS_EVAL_TYPE_GLOBAL);
  jsrt_stats.js_ns += plat_clock_ns() - t;
  if (JS_IsException(r)) {report(); return -1;}
  JS_FreeValue(ctx, r); return 0;
}
int jsrt_exit_code(void) {return exit_code;}
static JSClassID new_class(JSClassID *id, const char *name, JSClassFinalizer *fin, JSClassGCMark *mark, const JSCFunctionListEntry *fns, int n) {
  JS_NewClassID(rt, id);
  JSClassDef def = {name, fin, mark, 0, 0};
  JS_NewClass(rt, *id, &def);
  JSValue proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, proto, fns, n);
  JS_SetClassProto(ctx, *id, proto);
  return *id;
}
static void refuse(const char *why) {
  snprintf(start_who, sizeof start_who, "APPS/%.8s", app_id); snprintf(start_why, sizeof start_why, "%s", why);
  sayf("JS app %s refused: %s", app_id, why);
}
/* The app's manifest (APP.JSON) into caps, policy and the entry file; 0, or -1 with start_who/start_why set. */
static int load_app(const char *dir) {
  restricted = 0; caps = APP_CAP_ALL; strcpy(entry_name, "MAIN.JS"); memset(&manifest, 0, sizeof manifest);
  const char *id = dir ? app_id_of(dir) : 0;
  snprintf(app_id, sizeof app_id, "%s", id ? id : dir ? "APP" : "PICKER");
  if (!dir) return 0;
  char path[96], err[96]; snprintf(path, sizeof path, "%s/APP.JSON", dir);
  int32_t n = plat_read(path, io, APP_MANIFEST_MAX + 1);
  if (n < 0) {if (jsrt_require_manifest) {refuse("APP.JSON is missing"); return -1;} return 0;}
  if (app_manifest_parse((const char *)io, (uint32_t)n, &manifest, err, sizeof err)) {refuse(err); return -1;}
  if (manifest.api > APP_API_VERSION) {
    snprintf(err, sizeof err, "needs runtime API %u, this is %u", (unsigned)manifest.api, APP_API_VERSION);
    refuse(err); return -1;
  }
  restricted = 1; caps = manifest.caps; snprintf(entry_name, sizeof entry_name, "%s", manifest.entry);
  app_policy_init(&policy, app_id, caps);
  char list[104] = ""; size_t k = 0;
  for (unsigned i = 4; app_cap_name(i) && k < sizeof list; i++) if (caps & (1u << i)) k += (size_t)snprintf(list + k, sizeof list - k, "%s%s", k ? " " : "", app_cap_name(i));
  sayf("JS app %s %s api %u: %s", manifest.name, manifest.version, (unsigned)manifest.api, k ? list : "no extra capabilities");
  return 0;
}
int jsrt_start(const char *dir, uint32_t heap_limit) {
  native = JS_UNDEFINED;
  exited = exit_code = want_frame = 0;
  wake_at_ms = 1e300;
  in_picker = !dir; launch_dir[0] = 0; pending = PEND_NONE;
  game_el = 0; game_started = lowres_on = 0; game_dir[0] = 0;
  lowres = 1; game_w = GP_W; game_h = GP_H; font_scale = 1;
  if (PROF->init) PROF->init();
  prof_live = 1;
  orphan_mode = 0;
  if (load_app(dir)) return -1;
  memset(&jsrt_stats, 0, sizeof jsrt_stats);
  uint64_t t = plat_clock_ns();
  t0_ns = t;
  rt = JS_NewRuntime();
  if (!rt) {say("JS no runtime (memory)"); return -1;}
  JS_SetMemoryLimit(rt, heap_limit);
  JS_SetMaxStackSize(rt, 256 * 1024);
  ctx = JS_NewContext(rt);
  if (!ctx) {say("JS no context (memory)"); return -1;}
  for (unsigned i = 0; i < NSTYLE; i++) style_proto[i] = (JSCFunctionListEntry)JS_CGETSET_MAGIC_DEF(style_names[i], style_get, style_set, (int16_t)i);
  new_class(&el_class, "Element", el_finalizer, el_mark, el_proto, (int)(sizeof el_proto / sizeof el_proto[0]));
  new_class(&style_class, "CSSStyleDeclaration", style_finalizer, style_mark, style_proto, (int)NSTYLE);
  new_class(&ctx2d_class, "CanvasRenderingContext2D", style_finalizer, ctx2d_mark, ctx2d_proto, (int)(sizeof ctx2d_proto / sizeof ctx2d_proto[0]));
  if (set_mode(PLAT_W, PLAT_H, 32)) {say("JS cannot restore page mode"); return -1;}
  dom_init(load_picture, PROF->draw);
  native = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, native, native_fns, (int)(sizeof native_fns / sizeof native_fns[0]));
  jsfs_init(rt, ctx, native);
  jsdev_init(rt, ctx, native);
  jsaudio_init(ctx, native);
  jseink_init(ctx, native);
  jsserial_init(ctx, native);
  jspower_init(ctx, native);
  JSValue body = new_element(ctx, DOM_DIV); /* the root the tree draws from */
  struct jel *bj = jel_of(body);
  dom_delete(bj->e); bj->e = dom_body; dom_body->js = bj;
  JS_SetPropertyStr(ctx, native, "body", body);
  JSValue ector = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, ector, "prototype", JS_GetClassProto(ctx, el_class));
  JS_SetPropertyStr(ctx, native, "Element", ector);
  JSValue fn = JS_Eval(ctx, jsrt_prelude, jsrt_prelude_len, "prelude.js", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fn)) {report(); return -1;}
  JSValue g = JS_GetGlobalObject(ctx), args[2] = {g, native};
  JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
  JS_FreeValue(ctx, fn);
  if (JS_IsException(r)) {report(); JS_FreeValue(ctx, g); return -1;}
  JS_FreeValue(ctx, r);
  JS_SetPropertyStr(ctx, g, "Element", JS_DupValue(ctx, ector));
  JS_FreeValue(ctx, g);
  {JSMemoryUsage u0; JS_ComputeMemoryUsage(rt, &u0);
   sayf("JS runtime ready in %u us (engine, browser-like globals), heap %u bytes", (unsigned)((plat_clock_ns() - t) / 1000), (unsigned)u0.malloc_size);}
  if (!dir) {
    if (jsrt_eval(jsrt_picker, jsrt_picker_len, "picker.js")) return -1;
    next_frame_ns = plat_clock_ns();
    return 0;
  }
  char path[96]; snprintf(path, sizeof path, "%s/%s", dir, entry_name);
  char *src = (char *)io; /* JS_Eval copies what it keeps */
  int32_t n = plat_read(path, io, sizeof io - 1);
  if (n < 0) {sayf("JS no %s", path); snprintf(start_who, sizeof start_who, "APPS/%.8s", app_id); snprintf(start_why, sizeof start_why, "%s is not in the app folder", entry_name); return -1;}
  src[n] = 0;
  uint64_t before = jsrt_stats.js_ns;
  int bad = jsrt_eval(src, (uint32_t)n, path);
  jsrt_stats.js_ns = before;
  jsrt_stats.start_ns = plat_clock_ns() - t;
  JSMemoryUsage u; JS_ComputeMemoryUsage(rt, &u);
  sayf("JS started %s in %u us, heap %u bytes in %u blocks%s", path, (unsigned)(jsrt_stats.start_ns / 1000), (unsigned)u.malloc_size, (unsigned)u.malloc_count, bad ? " (it threw)" : "");
  next_frame_ns = plat_clock_ns();
  if (bad) {snprintf(start_who, sizeof start_who, "APPS/%.8s", app_id); snprintf(start_why, sizeof start_why, "%s threw an error (see the log)", entry_name);}
  return bad ? -1 : 0;
}
uint64_t jsrt_step(void) {
  uint16_t type, code; int32_t value;
  plat_heartbeat();
  if (exited) return 0;
  unsigned handled = 0, more = 0;
  while (!exited) { /* at most JSDEV_EVENTS_PER_STEP events a turn, so a flood cannot starve timers and frames */
    if (handled == JSDEV_EVENTS_PER_STEP) {more = 1; break;}
    if (!plat_event(&type, &code, &value)) break;
    handled++;
    /* F10 is owned by the host, before game routing or preventDefault(). */
    if (type == 1 && code == 68) {if (value == 1) {exited = 1; exit_code = 0;}}
    else {
      if (jsdev_input(type, code, value)) for (int k; (k = jsdev_released_next()) >= 0;) key_event((uint16_t)k, 0); /* a reset lets go of held keys */
      if (type == 1 && !exited) key_event(code, value);
    }
  }
  if (handled > jsrt_stats.input_max) jsrt_stats.input_max = handled;
  if (exited) return 0;
  uint64_t now = plat_clock_ns();
  jsdev_sleeps_due(now);
  jspower_pump(now); /* 'blank' and 'wake' events, only while a listener exists */
  jsserial_pump(now); /* the 'data' event: bounded reads */
  jseink_pump(now);   /* sends the newest queued e-ink update when the panel limits allow */
  jsaudio_pump(now); /* keeps the audio ring fed: onneed(frames), tones; bounded, never waits */
  if (now_ms() >= wake_at_ms) {
    wake_at_ms = 1e300;
    JSValue a = JS_NewFloat64(ctx, now_ms());
    JS_FreeValue(ctx, call_native("onTimers", 1, &a));
  }
  if (exited) return 0;
  if (want_frame && now >= next_frame_ns) {
    want_frame = 0;
    next_frame_ns += FRAME_NS;
    if (next_frame_ns < now) next_frame_ns = now + FRAME_NS; /* fell behind: no burst of catch-up frames */
    JSValue a = JS_NewFloat64(ctx, now_ms());
    JS_FreeValue(ctx, call_native("onFrame", 1, &a));
    jsrt_stats.frames++;
  }
  if (exited) return 0;
  if (game_started && PROF->step) {
    struct jsrt_direct d = {0, 0};
    uint32_t r = PROF->step(plat_clock_ns(), &d);
    if (r & JSRT_DRAW && lowres_on) { /* 320x200 indices as they are; the palette only when it changed */
      uint64_t d0 = plat_clock_ns();
      if (d.px && d.pal) {
        if (memcmp(shown_pal, d.pal, 768)) {memcpy(shown_pal, d.pal, 768); plat_palette(d.pal);}
        uint32_t ox = (game_w - GP_W) / 2, oy = (game_h - GP_H) / 2;
        for (uint32_t y = 0; y < GP_H; y++) memcpy((uint8_t *)plat_fb() + (oy + y) * game_w + ox, d.px + y * GP_W, GP_W);
        plat_present(ox, oy, GP_W, GP_H);
        jsrt_stats.present_ns += plat_clock_ns() - d0; jsrt_stats.presents++;
      }
    } else if (r & JSRT_DRAW && game_el) {struct dom_rect all = {0, 0, PLAT_W, PLAT_H}; dom_damage(&all);}
    if (r & JSRT_END) stop_game();
  }
  for (JSContext *c; !exited && JS_ExecutePendingJob(rt, &c) > 0;) {}
  if (exited) return 0;
  if (orphan_mode) {orphan_mode = 0; lowres_on = 0; set_mode(mode_w, mode_h, mode_d);}
  uint64_t t = plat_clock_ns();
  struct dom_rect rects[DOM_MAX_DIRTY];
  uint32_t n = lowres_on || jsdev_leased() ? 0 : dom_render(plat_fb(), rects); /* a leased screen is the app's, not the page's */
  uint64_t t2 = plat_clock_ns();
  jsrt_stats.render_ns += t2 - t;
  for (uint32_t i = 0; i < n; i++) plat_present((uint32_t)rects[i].x, (uint32_t)rects[i].y, (uint32_t)rects[i].w, (uint32_t)rects[i].h);
  jsrt_stats.present_ns += plat_clock_ns() - t2;
  jsrt_stats.presents += n;
  if (exited) return 0;
  now = plat_clock_ns();
  uint64_t wake = more ? now + 1 : jseink_wake(now, jsserial_wake(now, jspower_wake(now, jsaudio_wake(now, jsdev_wake(now))))); /* input every 10 ms (100 ms with no device), or an earlier sleep; never past the chunk */
  if (want_frame && next_frame_ns < wake) wake = next_frame_ns;
  if (wake_at_ms < 1e300) {uint64_t w = t0_ns + (uint64_t)(wake_at_ms * 1e6); if (w < wake) wake = w;}
  if (game_started && PROF->wake) {uint64_t w = PROF->wake(now); if (w && w < wake) wake = w;}
  return wake > now ? wake : now + 1;
}

/* Dispose the entire app realm, not merely its body. No JS shutdown callbacks:
   those could restart timers or block the return path. Device handles belong
   to JS.ELF and survive; queued events and the page's state do not. */
int jsrt_stop(void) {
  if (game_started && PROF->end) PROF->end();
  game_started = 0; game_el = 0;
  if (prof_live) {prof_live = 0; if (PROF->stop) PROF->stop();}
  char sum[80]; unsigned leaks = jsrt_handles_release_all(sum, sizeof sum); /* what the app did not close */
  if (leaks) sayf("JS app left %u handle(s) open: %s", leaks, sum);
  unsigned holds = jspower_release_all(); /* power holds are tokens, not handles */
  if (holds) {sayf("JS app left %u power hold(s)", holds); leaks += holds;}
  if (ctx) {JS_FreeValue(ctx, native); native = JS_UNDEFINED; JS_FreeContext(ctx); ctx = 0;}
  if (rt) {JS_FreeRuntime(rt); rt = 0;}
  dom_body = dom_focused = 0;
  want_frame = lowres_on = orphan_mode = 0; wake_at_ms = 1e300;
  plat_reset_input();
  return (int)leaks;
}
static int try_start(const char *dir, int fresh) { /* one start, no fallback; a failed start is stopped. fresh: forget why the last app did not start */
  if (fresh) start_who[0] = start_why[0] = 0;
  if (!jsrt_start(dir, app_heap_limit)) return 0;
  jsrt_stop(); return -1;
}
static void launch_state_clear(void) {ldepth = 0; cur_args[0] = fail_id[0] = ret_id[0] = 0; ret_set = 0;}
int jsapp_start(const char *dir, uint32_t heap_limit) {
  app_heap_limit = heap_limit;
  launch_state_clear();
  if (!try_start(dir, 1)) return 0;
  if (dir && !try_start(0, 0)) return 0;
  return -1;
}
/* The app asked to launch or exit: moves the stack and starts the next app; a launch that fails goes back to the caller with
   the reason, and a caller that will not start either falls to the picker. 0, or -1 when nothing would start. */
static int app_switch(const char *from_id) {
  char dir[64]; fail_id[0] = 0; ret_set = 0;
  if (pending == PEND_LAUNCH) {
    snprintf(lstack[ldepth].id, sizeof lstack[0].id, "%s", from_id); snprintf(lstack[ldepth].args, sizeof lstack[0].args, "%s", cur_args);
    ldepth++;
    snprintf(dir, sizeof dir, "/sd/OS/APPS/%s", pend_id);
    snprintf(cur_args, sizeof cur_args, "%s", pend_args);
    sayf("JS launch %s from %s, depth %u", pend_id, from_id, ldepth);
    if (!try_start(dir, 1)) return 0;
    snprintf(fail_id, sizeof fail_id, "%s", pend_id); /* back to the caller, with the reason */
    snprintf(fail_why, sizeof fail_why, "%s", start_why[0] ? start_why : "it did not start (see the log)");
    sayf("JS launch of %s failed: %s", fail_id, fail_why);
    ldepth--;
    snprintf(cur_args, sizeof cur_args, "%s", lstack[ldepth].args);
    snprintf(dir, sizeof dir, "/sd/OS/APPS/%s", lstack[ldepth].id);
    if (!try_start(dir, 1)) return 0;
  } else if (ldepth) {
    ldepth--; snprintf(ret_id, sizeof ret_id, "%s", from_id); ret_code = exit_code; ret_set = 1;
    snprintf(cur_args, sizeof cur_args, "%s", lstack[ldepth].args);
    snprintf(dir, sizeof dir, "/sd/OS/APPS/%s", lstack[ldepth].id);
    sayf("JS exit %s to %s, depth %u", from_id, lstack[ldepth].id, ldepth);
    if (!try_start(dir, 1)) return 0;
  } else if (autorun_dir[0] && strcmp(app_id_of(autorun_dir), from_id)) { /* nobody launched it: the AUTORUN app, or the picker */
    cur_args[0] = 0; ret_id[0] = 0;
    snprintf(ret_id, sizeof ret_id, "%s", from_id); ret_code = exit_code; ret_set = 1;
    sayf("JS exit %s to the AUTORUN app", from_id);
    if (!try_start(autorun_dir, 1)) return 0;
  }
  launch_state_clear();
  return try_start(0, 0);
}
uint64_t jsapp_step(void) {
  uint64_t wake = jsrt_step();
  if (wake) return wake;
  int picker = in_picker;
  char next[64]; snprintf(next, sizeof next, "%s", launch_dir);
  char from[9]; snprintf(from, sizeof from, "%s", app_id);
  int req = pending;
  if (!picker) sayf("JS app returned status %d", exit_code);
  jsrt_stop();
  sayf("JS app stopped");
  if (!picker && req != PEND_NONE) {
    if (app_switch(from)) {exit_code = 5; return 0;}
    return plat_clock_ns() + 1;
  }
  if (picker && !next[0]) return 0; /* quit host through existing entry return */
  if (jsapp_start(next[0] ? next : 0, app_heap_limit)) {exit_code = 5; return 0;}
  return plat_clock_ns() + 1;
}
int jsapp_autorun(uint32_t heap_limit) {
  static char text[512]; char dir[48], err[80];
  int32_t n = plat_read("/sd/OS/AUTORUN.TXT", (uint8_t *)text, sizeof text - 1);
  if (n < 0) n = 0;
  if (app_autorun_parse(text, (uint32_t)n, dir, sizeof dir, err, sizeof err)) {
    sayf("JS %s", err);
    app_heap_limit = heap_limit; /* the picker, with the reason on screen */
    snprintf(start_who, sizeof start_who, "AUTORUN.TXT"); snprintf(start_why, sizeof start_why, "%s", err);
    if (!jsrt_start(0, heap_limit)) return 0;
    jsrt_stop(); return -1;
  }
  sayf("JS autorun %s", dir[0] ? dir + 4 : "PICKER");
  snprintf(autorun_dir, sizeof autorun_dir, "%s", dir);
  return jsapp_start(dir[0] ? dir : 0, heap_limit);
}
