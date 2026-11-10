/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). See include/audio_null.h. */
#include "audio_null.h"
#include "clock.h"
struct audio_dev audio0;
void audio_dev_init(void) { audio_ring_init(&audio0.ring); audio0.open = 0; audio0.clocked = 0; audio0.frac = 0; audio0.wrote = 0; }
uint32_t audio_dev_write(const void *src, uint32_t bytes) {
    uint32_t n = audio_ring_write(&audio0.ring, src, bytes);
    if (n) audio0.wrote = 1;
    return n;
}
int audio_dev_open(void) {
    if (audio0.open) return -16;
    audio_ring_init(&audio0.ring); /* new counters for each open */
    audio0.open = 1; audio0.clocked = 0; audio0.frac = 0; audio0.wrote = 0;
    return 0;
}
void audio_dev_close(void) {
    if (!audio0.open) return;
    audio0.open = 0; audio_ring_flush(&audio0.ring);
}
void audio_null_tick(uint64_t now_us) {
    if (!audio0.open) { audio0.clocked = 0; return; }
    if (!audio0.clocked) { audio0.last_us = now_us; audio0.frac = 0; audio0.clocked = 1; return; }
    /* 32-bit math only (the kernel has no 64-bit divide): 48 frames per
       1000 us, the remainder carried in thousandths of a frame. A stall over
       a second is clamped to a second; it underruns either way. */
    uint64_t gap = now_us - audio0.last_us; audio0.last_us = now_us;
    uint32_t dt = gap > 1000000u ? 1000000u : (uint32_t)gap;
    uint32_t due = dt * (AUDIO_RING_RATE / 1000u) + (uint32_t)audio0.frac;
    audio0.frac = due % 1000u;
    uint32_t frames = due / 1000u;
    /* Underrun is real only once the app has started: nothing queued and
     * never written yet is not a glitch; count from the first write. */
    if (!audio0.wrote) return;
    static uint8_t scratch[1024u * AUDIO_FRAME_BYTES];
    while (frames) {
        uint32_t n = frames > 1024u ? 1024u : frames;
        audio_ring_read(&audio0.ring, scratch, n); /* the sink: dropped */
        frames -= n;
    }
}
void audio_null_task(void) { audio_null_tick(clock_us()); }
