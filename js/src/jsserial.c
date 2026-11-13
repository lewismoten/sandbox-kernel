/* SPDX-License-Identifier: MIT; serial for apps (slice 4d), see jsserial.h. One JSRT_H_SERIAL entry; natives take its id. The kernel
 * channel is nonblocking: read gives what is waiting (0 when empty), write gives how many bytes were taken. The runtime never
 * loops to finish a write. Direction (read, write) is enforced here: the kernel does not check it after open. */
#include "jsserial.h"
#include "app.h"
#include "handle.h"
#include "jsdev.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include "alloc.h"

#define IO_MAX 4096u
static JSContext *sctx;
struct se {uint32_t id, mode, want; uint64_t next_poll, rx, tx, events;};
static struct se *S;

static void se_release(void *u) {(void)u; plat_serial_close(); free(S); S = 0;}
static struct se *get(JSContext *c, JSValueConst v) {
  uint32_t id = 0;
  if (JS_ToUint32(c, &id, v)) return 0;
  struct se *s = S && S->id == id ? jsrt_handle_get(id, JSRT_H_SERIAL) : 0;
  if (!s) JS_ThrowPlainError(c, "EBADF: the serial port is closed");
  return s;
}
static const char *err_name(int e) {return e == -16 ? "EBUSY" : e == -19 ? "ENODEV" : e == -13 ? "EACCES" : e == -9 ? "EBADF" : "EIO";}
static JSValue throw_errno(JSContext *c, int e, const char *what) {return JS_ThrowPlainError(c, "%s: %s failed (%d)", err_name(e), what, e);}

/* open(mode) with bit 0 = read, bit 1 = write -> id */
static JSValue js_open(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; uint32_t mode = 3;
  if (jsrt_need(c, APP_CAP_SERIAL, "serial.open")) return JS_EXCEPTION;
  if (n > 0 && !JS_IsUndefined(v[0]) && JS_ToUint32(c, &mode, v[0])) return JS_EXCEPTION;
  if (mode < 1 || mode > 3) return JS_ThrowRangeError(c, "EINVAL: mode is 'r', 'w' or 'rw'");
  if (S) return JS_ThrowPlainError(c, "EBUSY: this app already has the serial port open");
  struct se *s = calloc(1, sizeof *s);
  if (!s) return JS_ThrowOutOfMemory(c);
  int e = plat_serial_open();
  if (e) {free(s); return throw_errno(c, e, "open /dev/serial0");}
  s->id = jsrt_handle_open(JSRT_H_SERIAL, se_release, s);
  if (!s->id) {plat_serial_close(); free(s); return JS_ThrowRangeError(c, "EMFILE: too many open handles");}
  s->mode = mode; S = s;
  return JS_NewUint32(c, s->id);
}
static JSValue js_close(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; uint32_t id = 0;
  if (n > 0 && JS_ToUint32(c, &id, v[0])) return JS_EXCEPTION;
  if (S && S->id == id) jsrt_handle_close(id);
  return JS_UNDEFINED;
}
/* The typed array's bytes (a Uint8Array view), or 0 with an exception. */
static uint8_t *bytes_of(JSContext *c, JSValueConst a, size_t *len, const char *what) {
  if (JS_GetTypedArrayType(a) != JS_TYPED_ARRAY_UINT8) {JS_ThrowTypeError(c, "%s: a Uint8Array", what); return 0;}
  size_t off, bpe; JSValue ab = JS_GetTypedArrayBuffer(c, a, &off, len, &bpe);
  if (JS_IsException(ab)) return 0;
  size_t asz; uint8_t *base = JS_GetArrayBuffer(c, &asz, ab);
  JS_FreeValue(c, ab);
  if (!base || off + *len > asz) {JS_ThrowPlainError(c, "EBADF: the buffer was detached"); return 0;}
  return base + off;
}
/* read(id, Uint8Array) -> bytes read now (0 when nothing is waiting); at most 4096 a call */
static JSValue js_read(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct se *s = n > 0 ? get(c, v[0]) : 0;
  if (!s) return JS_EXCEPTION;
  if (!(s->mode & 1)) return JS_ThrowPlainError(c, "EBADF: the port was opened for writing only");
  size_t len; uint8_t *p = n > 1 ? bytes_of(c, v[1], &len, "serial.read") : (JS_ThrowTypeError(c, "serial.read: a Uint8Array"), (uint8_t *)0);
  if (!p) return JS_EXCEPTION;
  uint32_t got = 0, want = len > IO_MAX ? IO_MAX : (uint32_t)len;
  if (!want) return JS_NewInt32(c, 0);
  int e = plat_serial_read(p, want, &got);
  if (e) return throw_errno(c, e, "read /dev/serial0");
  s->rx += got; return JS_NewUint32(c, got);
}
/* write(id, Uint8Array) -> bytes taken (maybe fewer than given, maybe 0); at most 4096 a call */
static JSValue js_write(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct se *s = n > 0 ? get(c, v[0]) : 0;
  if (!s) return JS_EXCEPTION;
  if (!(s->mode & 2)) return JS_ThrowPlainError(c, "EBADF: the port was opened for reading only");
  size_t len; uint8_t *p = n > 1 ? bytes_of(c, v[1], &len, "serial.write") : (JS_ThrowTypeError(c, "serial.write: a Uint8Array"), (uint8_t *)0);
  if (!p) return JS_EXCEPTION;
  uint32_t took = 0, give = len > IO_MAX ? IO_MAX : (uint32_t)len;
  if (!give) return JS_NewInt32(c, 0);
  int e = plat_serial_write(p, give, &took);
  if (e) return throw_errno(c, e, "write /dev/serial0");
  s->tx += took; return JS_NewUint32(c, took);
}
static JSValue js_stats(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct se *s = n > 0 ? get(c, v[0]) : 0;
  if (!s) return JS_EXCEPTION;
  struct plat_serial_info q;
  int e = plat_serial_query(&q);
  if (e) return throw_errno(c, e, "query /dev/serial0");
  JSValue o = JS_NewObject(c);
#define U(k, x) JS_SetPropertyStr(c, o, k, JS_NewInt64(c, (int64_t)(x)))
  U("transport", q.transport); U("txBytes", q.tx_bytes); U("txDropped", q.tx_dropped); U("rxBytes", q.rx_bytes);
  U("rxDropped", q.rx_dropped); U("txPending", q.tx_pending); U("rxPending", q.rx_pending);
  U("appRead", s->rx); U("appWritten", s->tx); U("events", s->events);
  return o;
#undef U
}
static JSValue js_want(JSContext *c, JSValueConst t, int n, JSValueConst *v) { /* want(id, bool): is anyone listening for 'data' */
  (void)t; struct se *s = n >= 2 ? get(c, v[0]) : 0;
  if (!s) return n >= 2 ? JS_EXCEPTION : JS_ThrowTypeError(c, "serial: want(id, on)");
  if (!(s->mode & 1) && JS_ToBool(c, v[1])) return JS_ThrowPlainError(c, "EBADF: the port was opened for writing only");
  s->want = JS_ToBool(c, v[1]) != 0; return JS_UNDEFINED;
}
void jsserial_pump(uint64_t now) {
  struct se *s = S;
  if (!s || !s->want || now < s->next_poll) return;
  s->next_poll = now + JSSERIAL_POLL_NS;
  uint8_t buf[JSSERIAL_CHUNK];
  for (uint32_t total = 0; total < JSSERIAL_PER_STEP;) {
    uint32_t got = 0;
    if (plat_serial_read(buf, sizeof buf, &got) || !got) break;
    total += got; s->rx += got; s->events++;
    JSValue args[2] = {JS_NewUint32(sctx, s->id), JS_NewArrayBufferCopy(sctx, buf, got)};
    JS_FreeValue(sctx, jsrt_call_native("onSerial", 2, args));
    if (S != s || !s->want) break; /* closed or unsubscribed in the handler */
    if (got < sizeof buf) break;   /* drained: nothing more waiting */
  }
}
uint64_t jsserial_wake(uint64_t now, uint64_t wake) {
  if (S && S->want) {uint64_t w = S->next_poll > now ? S->next_poll : now + 1; if (w < wake) wake = w;}
  return wake;
}
static const JSCFunctionListEntry fns[] = {
  JS_CFUNC_DEF("serialOpen", 1, js_open), JS_CFUNC_DEF("serialClose", 1, js_close), JS_CFUNC_DEF("serialRead", 2, js_read),
  JS_CFUNC_DEF("serialWrite", 2, js_write), JS_CFUNC_DEF("serialStats", 1, js_stats), JS_CFUNC_DEF("serialWant", 2, js_want),
};
void jsserial_init(JSContext *c, JSValueConst native) {
  sctx = c; S = 0;
  JS_SetPropertyFunctionList(c, native, fns, (int)(sizeof fns / sizeof fns[0]));
}
