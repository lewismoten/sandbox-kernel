/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). See include/mixer.h. */
#include <string.h>
#include "audio_null.h"
#include "mixer.h"

static uint32_t hz_step(uint32_t hz) { /* hz * 2^32 / 48000 = hz*89478.485; hz <= 20000 fits in 32 bits */
    return hz * 89478u + (hz * 485u) / 1000u;
}
static int hz_ok(uint32_t hz) { return hz >= SPEAKER_MIN_HZ; }
static uint32_t clamp_hz(uint32_t hz) { return hz > SPEAKER_MAX_HZ ? SPEAKER_MAX_HZ : hz; }

void mixer_init(struct mixer *m) { memset(m, 0, sizeof *m); }

static int alloc(struct mixer *m) {
    for (int i = 0; i < MIXER_VOICES; i++) if (!m->v[i].active) return i;
    return -1;
}
int mixer_tone(struct mixer *m, uint32_t hz, uint32_t ms, uint16_t vol) {
    if (!hz_ok(hz)) return -1;
    int i = alloc(m); if (i < 0) return -1;
    struct mixer_voice *v = &m->v[i];
    memset(v, 0, sizeof *v);
    v->active = 1; v->vol = vol; v->step = hz_step(clamp_hz(hz));
    v->remaining = ms ? (uint64_t)ms * (AUDIO_RING_RATE / 1000u) : 0; /* 48 frames a ms */
    return i;
}
int mixer_tune(struct mixer *m, const uint16_t *freqs, uint32_t n, uint16_t vol) {
    if (!freqs || !n) return -1;
    int i = alloc(m); if (i < 0) return -1;
    struct mixer_voice *v = &m->v[i];
    memset(v, 0, sizeof *v);
    v->active = 1; v->vol = vol; v->freqs = freqs; v->nfreqs = n;
    return i;
}
void mixer_stop(struct mixer *m, int i) { if (i >= 0 && i < MIXER_VOICES) m->v[i].active = 0; }
void mixer_set_volume(struct mixer *m, int i, uint16_t vol) { if (i >= 0 && i < MIXER_VOICES) m->v[i].vol = vol; }
uint32_t mixer_active(const struct mixer *m) {
    uint32_t n = 0; for (int i = 0; i < MIXER_VOICES; i++) n += m->v[i].active; return n;
}

/* Tick index of frame f: floor(f * 1193180 / (8191 * 48000)). Avoids a divide by
 * advancing `tick` while the next tick's start frame is <= f (compared by cross-multiplying). */
static int tick_due(uint64_t frames, uint32_t next_tick) {
    return frames * SPEAKER_TICK_NUM >= (uint64_t)next_tick * SPEAKER_TICK_DIV * AUDIO_RING_RATE;
}

static void render_voice(struct mixer_voice *v, int32_t *acc, uint32_t frames) {
    int32_t amp = (int32_t)((32767u * (uint32_t)v->vol) >> 8);
    for (uint32_t i = 0; i < frames && v->active; i++) {
        uint32_t step = v->step;
        if (v->freqs) {
            while (v->tick < v->nfreqs && tick_due(v->frames, v->tick + 1)) v->tick++;
            if (v->tick >= v->nfreqs) { v->active = 0; break; }
            uint32_t hz = v->freqs[v->tick];
            step = hz_ok(hz) ? hz_step(clamp_hz(hz)) : 0;
            amp = (int32_t)((32767u * SPEAKER_GAIN * (uint32_t)v->vol) >> 16);
        }
        if (step) { /* step 0 = silent tick; phase keeps */
            v->phase += step;
            acc[i] += (v->phase & 0x80000000u) ? -amp : amp;
        }
        v->frames++;
        if (!v->freqs && v->remaining && --v->remaining == 0) v->active = 0;
    }
}

void mixer_render(struct mixer *m, uint32_t *out, uint32_t frames) {
    for (uint32_t base = 0; base < frames; base += 256) {
        uint32_t n = frames - base < 256 ? frames - base : 256;
        int32_t acc[256];
        memset(acc, 0, n * sizeof acc[0]);
        for (int k = 0; k < MIXER_VOICES; k++) if (m->v[k].active) render_voice(&m->v[k], acc, n);
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = acc[i];
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            uint32_t u = (uint16_t)s;
            out[base + i] = u | (u << 16);
        }
    }
}

static uint32_t pump(struct mixer *m, struct audio_ring *r, uint32_t max, int dev) {
    uint32_t want = audio_ring_free(r);
    if (want > max) want = max;
    uint32_t buf[256], done = 0;
    while (done < want) {
        uint32_t n = want - done < 256 ? want - done : 256;
        mixer_render(m, buf, n);
        uint32_t q = dev ? audio_dev_write(buf, n * AUDIO_FRAME_BYTES) : audio_ring_write(r, buf, n * AUDIO_FRAME_BYTES);
        done += q;
        if (q < n) break;
    }
    return done;
}
uint32_t mixer_pump(struct mixer *m, struct audio_ring *r, uint32_t max) { return pump(m, r, max, 0); }
uint32_t mixer_pump_dev(struct mixer *m, uint32_t max) { return pump(m, &audio0.ring, max, 1); }
