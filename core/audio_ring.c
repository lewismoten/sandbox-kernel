/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). See include/audio_ring.h for the rules. */
#include "audio_ring.h"
#include <string.h>
#define LD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ST(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
static void sat_add(uint64_t *c, uint64_t n) { *c = *c + n < *c ? UINT64_MAX : *c + n; }
void audio_ring_init(struct audio_ring *r) { memset(r, 0, sizeof *r); }
uint32_t audio_ring_queued(const struct audio_ring *r) { return LD(&r->wr) - LD(&r->rd); }
uint32_t audio_ring_free(const struct audio_ring *r) { return AUDIO_RING_FRAMES - audio_ring_queued(r); }
void audio_ring_refuse(struct audio_ring *r, uint32_t frames) { if (frames) { sat_add(&r->overruns, 1); sat_add(&r->overrun_frames, frames); } }
uint32_t audio_ring_write(struct audio_ring *r, const void *src, uint32_t bytes) {
    uint32_t want = bytes / AUDIO_FRAME_BYTES, room = audio_ring_free(r);
    uint32_t n = want < room ? want : room;
    uint32_t wr = r->wr; /* only the producer writes it */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f; memcpy(&f, (const uint8_t *)src + i * 4u, 4);
        r->pcm[(wr + i) & (AUDIO_RING_FRAMES - 1u)] = f;
    }
    ST(&r->wr, wr + n); /* publish after the samples are in place */
    if (n < want) audio_ring_refuse(r, want - n);
    return n;
}
uint32_t audio_ring_read(struct audio_ring *r, void *out, uint32_t frames) {
    uint32_t have = audio_ring_queued(r), n = have < frames ? have : frames;
    uint32_t rd = r->rd; /* only the consumer writes it */
    uint8_t *o = out;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = r->pcm[(rd + i) & (AUDIO_RING_FRAMES - 1u)];
        memcpy(o + i * 4u, &f, 4);
    }
    memset(o + n * 4u, 0, (size_t)(frames - n) * 4u);
    ST(&r->rd, rd + n);
    sat_add(&r->played, n);
    if (n < frames) { sat_add(&r->underruns, 1); sat_add(&r->silence_frames, frames - n); }
    return n;
}
void audio_ring_flush(struct audio_ring *r) { ST(&r->rd, LD(&r->wr)); }
