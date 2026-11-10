/* SPDX-License-Identifier: MIT; native checks of the real core/audio_ring.c and
 * core/audio_null.c: ordering, wrap, silence, overrun, pacing, and a two-thread
 * single-producer/single-consumer stress for the lock-free claim. */
#include "clock.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "audio_null.h"
uint64_t clock_us(void) { return 0; } /* the tests pass timestamps themselves */
static struct audio_ring r;
static void frames(uint32_t *p, uint32_t from, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = from + i; }
static uint32_t buf[AUDIO_RING_FRAMES * 2];
static void ordering(void) {
    audio_ring_init(&r);
    uint32_t next = 0, got[700], src[700];
    for (int round = 0; round < 50; round++) { /* wraps the 4096-frame ring many times */
        frames(src, next, 700);
        assert(audio_ring_write(&r, src, 2800) == 700);
        assert(audio_ring_read(&r, got, 700) == 700);
        for (uint32_t i = 0; i < 700; i++) assert(got[i] == next + i);
        next += 700;
    }
    assert(r.underruns == 0 && r.played == 35000);
}
static void silence(void) {
    audio_ring_init(&r);
    uint32_t src[10], out[20]; frames(src, 1, 10);
    audio_ring_write(&r, src, 40);
    memset(out, 0xAA, sizeof out);
    assert(audio_ring_read(&r, out, 20) == 10);
    for (int i = 0; i < 10; i++) assert(out[i] == (uint32_t)(i + 1));
    for (int i = 10; i < 20; i++) assert(out[i] == 0); /* zeros, never stale data */
    assert(r.underruns == 1 && r.silence_frames == 10);
    assert(audio_ring_read(&r, out, 4) == 0 && out[0] == 0 && r.underruns == 2);
    assert(audio_ring_read(&r, out, 0) == 0 && r.underruns == 2); /* asking for nothing is no underrun */
}
static void overrun(void) {
    audio_ring_init(&r);
    assert(audio_ring_write(&r, buf, (AUDIO_RING_FRAMES - 1) * 4) == AUDIO_RING_FRAMES - 1);
    assert(audio_ring_write(&r, buf, 3 * 4 + 3) == 1); /* partial at a whole frame; stray bytes ignored */
    assert(r.overruns == 1 && r.overrun_frames == 2);
    assert(audio_ring_write(&r, buf, 4) == 0 && audio_ring_queued(&r) == AUDIO_RING_FRAMES && r.overruns == 2);
    uint32_t one;
    audio_ring_read(&r, &one, 1);
    assert(audio_ring_free(&r) == 1);
    audio_ring_flush(&r);
    assert(audio_ring_queued(&r) == 0);
}
static void pacing(void) {
    audio_dev_init();
    assert(audio_dev_open() == 0 && audio_dev_open() == -16);
    uint32_t src[AUDIO_RING_FRAMES]; frames(src, 0, AUDIO_RING_FRAMES);
    audio_null_tick(5000000); /* start the clock */
    assert(audio_dev_write(src, AUDIO_RING_FRAMES * 4) == AUDIO_RING_FRAMES);
    uint64_t t = 5000000;
    for (int i = 0; i < 1000; i++) { t += 1000; audio_null_tick(t); } /* 1 s in 1 ms steps */
    /* 1 s at 48 kHz is 48000 frames due; only 4096 existed: all played, rest underran */
    assert(audio0.ring.played == AUDIO_RING_FRAMES && audio0.ring.underruns > 0);
    assert(audio0.ring.played + audio0.ring.silence_frames == 48000);
    audio_dev_close();
    uint64_t before = audio0.ring.underruns;
    audio_null_tick(t + 1000000);
    assert(audio0.ring.underruns == before); /* closed: idle */
}
/* One writer thread, one reader thread: every frame arrives once, in order. */
#define TOTAL 2000000u
static void *producer_exact(void *a) {
    (void)a; uint32_t n = 0, chunk[37];
    while (n < TOTAL) {
        uint32_t want = TOTAL - n < 37 ? TOTAL - n : 37; frames(chunk, n, want);
        n += audio_ring_write(&r, chunk, want * 4);
    }
    return 0;
}
static void spsc(void) {
    audio_ring_init(&r);
    pthread_t t; pthread_create(&t, 0, producer_exact, 0);
    uint32_t expect = 0, out[61];
    while (expect < TOTAL) {
        uint32_t got = audio_ring_read(&r, out, 61);
        for (uint32_t i = 0; i < got; i++) assert(out[i] == expect + i);
        expect += got;
    }
    pthread_join(t, 0);
    assert(audio_ring_queued(&r) == 0);
}
/* The producer stops (closes, faults or exits): the queue drains once, then every frame read is zero, however long
   the consumer keeps reading, and each short read counts one underrun. No old frame is ever played twice. */
static void producer_stops(void) {
    struct audio_ring q; audio_ring_init(&q);
    uint32_t in[1000], out[384]; frames(in, 1, 1000);
    assert(audio_ring_write(&q, in, sizeof in) == 1000);
    uint32_t got = 0, nonzero_after = 0, reads = 0;
    while (reads++ < 200) { /* 200 segments of 384 frames (1.6 s) */
        uint32_t n = audio_ring_read(&q, out, 384); got += n;
        for (uint32_t i = n; i < 384; i++) nonzero_after += out[i] != 0;
    }
    assert(got == 1000 && nonzero_after == 0 && q.underruns == 198 && audio_ring_queued(&q) == 0);
    /* within 384 frames (8 ms) of the last real frame everything is silence */
    assert(q.silence_frames == 200u * 384u - 1000u);
}
int main(void) { producer_stops(); ordering(); silence(); overrun(); pacing(); spsc(); puts("audio ring ok"); return 0; }
