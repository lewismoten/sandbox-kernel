/* SPDX-License-Identifier: MIT; audio for apps (slice 4c), see jsaudio.h. One JSRT_H_AUDIO entry holds the state; the natives
 * take its id so a stale or closed device is EBADF. The kernel's format is fixed (stereo S16, 48 kHz), so the runtime converts:
 * mono is doubled, a rate that divides 48000 is repeated sample by sample, and a software volume scales (the kernel has none).
 * A write queues only what the ring has room for, in whole app frames, and returns that count. Nothing here waits. */
#include "jsaudio.h"
#include "app.h"
#include "handle.h"
#include "jsdev.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>

#define STAGE 1024u    /* kernel frames converted at a time (4 KiB on the stack) */
#define TONES_MAX 32u
#define MIN_REFILL 480u /* the pump does not call onneed for less than 10 ms of room */
#define KRATE 48000u
static JSContext *actx;
struct tone {uint32_t step, left, amp;};
struct au {
  uint32_t id, factor, channels, target, vol, muted, want;
  struct tone t[TONES_MAX]; unsigned nt; uint32_t phase;
  uint64_t next_poll, fed;
  uint64_t writes, refused, frames_in, refills, gaps, tone_frames;
};
static struct au *A;

static void au_release(void *u) {(void)u; plat_audio_close(); free(A); A = 0;}
static struct au *get(JSContext *c, JSValueConst v) {
  uint32_t id = 0;
  if (JS_ToUint32(c, &id, v)) return 0;
  struct au *a = A && A->id == id ? jsrt_handle_get(id, JSRT_H_AUDIO) : 0;
  if (!a) JS_ThrowPlainError(c, "EBADF: the audio device is closed");
  return a;
}
static const char *err_name(int e) {return e == -16 ? "EBUSY" : e == -19 ? "ENODEV" : e == -13 ? "EACCES" : e == -9 ? "EBADF" : "EIO";}
static JSValue throw_errno(JSContext *c, int e, const char *what) {return JS_ThrowPlainError(c, "%s: %s failed (%d)", err_name(e), what, e);}
static uint32_t gain(const struct au *a) {return a->muted ? 0 : a->vol;}

/* open(rate, channels, targetMs) -> id */
static JSValue js_open(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; uint32_t rate = KRATE, ch = 2, ms = JSAUDIO_TARGET_MS;
  if (jsrt_need(c, APP_CAP_AUDIO, "audio.open")) return JS_EXCEPTION;
  if ((n > 0 && !JS_IsUndefined(v[0]) && JS_ToUint32(c, &rate, v[0])) || (n > 1 && !JS_IsUndefined(v[1]) && JS_ToUint32(c, &ch, v[1])) ||
      (n > 2 && !JS_IsUndefined(v[2]) && JS_ToUint32(c, &ms, v[2]))) return JS_EXCEPTION;
  if (rate < 8000 || rate > KRATE || KRATE % rate) return JS_ThrowRangeError(c, "EINVAL: rate %u (the kernel plays 48000; a rate that divides it, from 8000, is repeated up)", (unsigned)rate);
  if (ch != 1 && ch != 2) return JS_ThrowRangeError(c, "EINVAL: channels must be 1 or 2");
  if (ms < 20 || ms > 300) return JS_ThrowRangeError(c, "EINVAL: targetMs 20 to 300");
  if (A) return JS_ThrowPlainError(c, "EBUSY: this app already has the audio device open");
  struct au *a = calloc(1, sizeof *a);
  if (!a) return JS_ThrowOutOfMemory(c);
  int e = plat_audio_open();
  if (e) {free(a); return throw_errno(c, e, "open /dev/audio");}
  a->id = jsrt_handle_open(JSRT_H_AUDIO, au_release, a);
  if (!a->id) {plat_audio_close(); free(a); return JS_ThrowRangeError(c, "EMFILE: too many open handles");}
  a->factor = KRATE / rate; a->channels = ch; a->target = ms * 48u; a->vol = 256;
  A = a;
  return JS_NewUint32(c, a->id);
}
static JSValue js_close(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; uint32_t id = 0;
  if (n > 0 && JS_ToUint32(c, &id, v[0])) return JS_EXCEPTION;
  if (A && A->id == id) jsrt_handle_close(id); /* closing twice is fine */
  return JS_UNDEFINED;
}

/* Converts app frames to kernel frames and queues them; returns app frames taken (a free-running ring has only room to grow). */
static uint32_t feed(struct au *a, const int16_t *in, uint32_t frames, int *err) {
  struct plat_audio_info q; int16_t out[STAGE * 2];
  uint32_t g = gain(a), per = STAGE / a->factor, done = 0;
  *err = plat_audio_query(&q);
  if (*err) return 0;
  uint64_t room = (q.capacity - q.queued) / a->factor;
  if (frames > room) frames = (uint32_t)room;
  while (done < frames) {
    uint32_t n = frames - done < per ? frames - done : per, o = 0;
    for (uint32_t i = 0; i < n; i++) {
      int32_t l = in[(done + i) * a->channels], r = in[(done + i) * a->channels + (a->channels - 1)];
      if (g != 256) {l = l * (int32_t)g >> 8; r = r * (int32_t)g >> 8;}
      for (uint32_t k = 0; k < a->factor; k++) {out[o * 2] = (int16_t)l; out[o * 2 + 1] = (int16_t)r; o++;}
    }
    uint32_t took = 0;
    *err = plat_audio_write(out, o, &took);
    done += took / a->factor;
    if (*err || took < o) break;
  }
  return done;
}
static JSValue js_write(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct au *a = n > 0 ? get(c, v[0]) : 0;
  if (!a) return JS_EXCEPTION;
  if (n < 2 || JS_GetTypedArrayType(v[1]) != JS_TYPED_ARRAY_INT16) return JS_ThrowTypeError(c, "audio.write: an Int16Array of interleaved samples");
  size_t off, len, bpe; JSValue ab = JS_GetTypedArrayBuffer(c, v[1], &off, &len, &bpe);
  if (JS_IsException(ab)) return ab;
  size_t asz; uint8_t *base = JS_GetArrayBuffer(c, &asz, ab);
  JS_FreeValue(c, ab);
  if (!base || off + len > asz) return JS_ThrowPlainError(c, "EBADF: the sample buffer was detached");
  if (len % (2u * a->channels)) return JS_ThrowRangeError(c, "EINVAL: %u samples is not whole frames of %u channel(s)", (unsigned)(len / 2), (unsigned)a->channels);
  uint32_t frames = (uint32_t)(len / (2u * a->channels)), took = 0;
  a->writes++;
  if (a->nt) a->refused++; /* tones own the ring until they finish */
  else {
    int e; took = feed(a, (const int16_t *)(base + off), frames, &e);
    if (e) return throw_errno(c, e, "write /dev/audio");
    if (took < frames) a->refused++;
    a->fed += took; a->frames_in += took;
  }
  return JS_NewUint32(c, took);
}

static JSValue js_stats(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct au *a = n > 0 ? get(c, v[0]) : 0;
  if (!a) return JS_EXCEPTION;
  struct plat_audio_info q;
  int e = plat_audio_query(&q);
  if (e) return throw_errno(c, e, "query /dev/audio");
  JSValue o = JS_NewObject(c);
#define U(k, x) JS_SetPropertyStr(c, o, k, JS_NewInt64(c, (int64_t)(x)))
  U("rate", q.rate); U("capacity", q.capacity); U("queued", q.queued); U("played", q.played);
  U("underruns", q.underruns); U("overruns", q.overruns); U("overrunFrames", q.overrun_frames);
  U("appRate", KRATE / a->factor); U("channels", a->channels); U("pendingTones", a->nt);
  U("writes", a->writes); U("refused", a->refused); U("framesIn", a->frames_in); U("refills", a->refills); U("gaps", a->gaps);
  return o;
#undef U
}
/* set(id, volumeQ8, muted, wantsRefill) */
static JSValue js_set(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct au *a = n >= 4 ? get(c, v[0]) : 0;
  if (!a) return n >= 4 ? JS_EXCEPTION : JS_ThrowTypeError(c, "audio: set(id, volume, muted, want)");
  uint32_t vol, m, w;
  if (JS_ToUint32(c, &vol, v[1]) || JS_ToUint32(c, &m, v[2]) || JS_ToUint32(c, &w, v[3])) return JS_EXCEPTION;
  a->vol = vol > 256 ? 256 : vol; a->muted = m != 0; a->want = w != 0;
  return JS_UNDEFINED;
}
/* tone(id, hz, ms, volumeQ8): queued after the tones before it; hz 0 is a rest. */
static JSValue js_tone(JSContext *c, JSValueConst t, int n, JSValueConst *v) {
  (void)t; struct au *a = n >= 4 ? get(c, v[0]) : 0;
  if (!a) return n >= 4 ? JS_EXCEPTION : JS_ThrowTypeError(c, "audio: tone(id, hz, ms, volume)");
  uint32_t hz, ms, vq;
  if (JS_ToUint32(c, &hz, v[1]) || JS_ToUint32(c, &ms, v[2]) || JS_ToUint32(c, &vq, v[3])) return JS_EXCEPTION;
  if (hz && (hz < 20 || hz > 20000)) return JS_ThrowRangeError(c, "EINVAL: tone hz is 0 (a rest) or 20 to 20000");
  if (ms < 1 || ms > 60000) return JS_ThrowRangeError(c, "EINVAL: tone ms 1 to 60000");
  if (a->nt == TONES_MAX) return JS_ThrowRangeError(c, "EMFILE: %u tones are already queued", (unsigned)TONES_MAX);
  /* phase step = hz * 2^32 / 48000 (an exact 64-bit product, one division at queue time) */
  a->t[a->nt++] = (struct tone){(uint32_t)((uint64_t)hz * 4294967296ull / KRATE), ms * 48u, hz ? 8192u * (vq > 256 ? 256 : vq) >> 8 : 0};
  return JS_UNDEFINED;
}
static JSValue js_stop(JSContext *c, JSValueConst t, int n, JSValueConst *v) { /* forgets the tones not yet synthesized; the ring plays on */
  (void)t; struct au *a = n > 0 ? get(c, v[0]) : 0;
  if (!a) return JS_EXCEPTION;
  a->nt = 0; return JS_UNDEFINED;
}

/* Synthesizes square-wave tones into the ring, at most `budget` kernel frames; stops at a full ring. */
static void synth(struct au *a, uint32_t budget) {
  int16_t out[STAGE * 2];
  uint32_t g = gain(a);
  while (a->nt && budget) {
    struct tone *t = &a->t[0];
    uint32_t n = t->left < STAGE ? t->left : STAGE;
    if (n > budget) n = budget;
    int32_t amp = (int32_t)(t->amp * g >> 8);
    for (uint32_t i = 0; i < n; i++) {
      int16_t s = t->amp ? (int16_t)(a->phase & 0x80000000u ? -amp : amp) : 0;
      a->phase += t->step; out[i * 2] = out[i * 2 + 1] = s;
    }
    uint32_t took = 0;
    if (plat_audio_write(out, n, &took)) return;
    t->left -= took; budget -= took; a->tone_frames += took; a->fed += took;
    if (!t->left) {memmove(a->t, a->t + 1, (a->nt - 1) * sizeof a->t[0]); a->nt--; a->phase = 0;}
    if (took < n) return; /* the ring is full: the rest of this tone waits for the next look */
  }
}
void jsaudio_pump(uint64_t now) {
  struct au *a = A;
  if (!a || now < a->next_poll || (!a->nt && !a->want)) return;
  a->next_poll = now + JSAUDIO_POLL_NS;
  struct plat_audio_info q;
  if (plat_audio_query(&q)) return;
  uint64_t need = q.queued < a->target ? a->target - q.queued : 0;
  if (need > JSAUDIO_PER_STEP) need = JSAUDIO_PER_STEP;
  if (a->nt) {synth(a, (uint32_t)need); return;}
  if (need < MIN_REFILL) return;
  if (a->fed && !q.queued) a->gaps++; /* the ring ran dry before this look: the kernel counted an underrun */
  a->refills++;
  JSValue args[2] = {JS_NewUint32(actx, a->id), JS_NewUint32(actx, (uint32_t)(need / a->factor))};
  JS_FreeValue(actx, jsrt_call_native("onAudio", 2, args));
}
uint64_t jsaudio_wake(uint64_t now, uint64_t wake) {
  if (A && (A->nt || A->want)) {
    uint64_t w = A->next_poll > now ? A->next_poll : now + 1;
    if (w < wake) wake = w;
  }
  return wake;
}
static const JSCFunctionListEntry fns[] = {
  JS_CFUNC_DEF("audioOpen", 3, js_open), JS_CFUNC_DEF("audioClose", 1, js_close), JS_CFUNC_DEF("audioWrite", 2, js_write),
  JS_CFUNC_DEF("audioStats", 1, js_stats), JS_CFUNC_DEF("audioSet", 4, js_set), JS_CFUNC_DEF("audioTone", 4, js_tone),
  JS_CFUNC_DEF("audioStop", 1, js_stop),
};
void jsaudio_init(JSContext *c, JSValueConst native) {
  actx = c; A = 0;
  JS_SetPropertyFunctionList(c, native, fns, (int)(sizeof fns / sizeof fns[0]));
}
