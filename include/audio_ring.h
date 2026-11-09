/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). The /dev/audio ring: one producer (the
 * syscall path), one consumer (a backend drained from a timer tick).
 *
 * Format: stereo, S16_LE, interleaved, AUDIO_RING_RATE frames a second, fixed.
 * Lock-free single producer / single consumer: the producer owns `wr`, the
 * consumer owns `rd`; each is a free-running frame count read by the other
 * side with acquire/release ordering. No call takes a lock.
 * Underrun: the consumer asks for more frames than are queued; the missing
 * frames are written as silence (zeros, never stale data) and each short
 * drain adds one to `underruns` (the frames missed go to `silence_frames`).
 * Overrun: the ring never overwrites unread audio. A write that does not fit
 * is cut at the last whole frame that does; refused frames add to
 * `overrun_frames` and one to `overruns`. The caller sees the short count.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define AUDIO_RING_RATE 48000u
#define AUDIO_RING_FRAMES 4096u /* power of two: about 85 ms */
#define AUDIO_FRAME_BYTES 4u
struct audio_ring {
    uint32_t wr, rd; /* free-running frame counts; queued = wr - rd */
    uint32_t pcm[AUDIO_RING_FRAMES]; /* one stereo frame per word */
    uint64_t played, underruns, silence_frames, overruns, overrun_frames;
};
void audio_ring_init(struct audio_ring *r);
/* Producer: copy up to `bytes` (a multiple of 4) from src. Returns the whole
 * frames queued (0..bytes/4); partial when the ring is nearly full. */
uint32_t audio_ring_write(struct audio_ring *r, const void *src, uint32_t bytes);
/* Consumer: fill `frames` frames into out; returns how many were real audio
 * (the rest are zero) and counts an underrun if that is less than frames. */
uint32_t audio_ring_read(struct audio_ring *r, void *out, uint32_t frames);
/* Producer: record frames a caller could not queue (one overrun event). */
void audio_ring_refuse(struct audio_ring *r, uint32_t frames);
uint32_t audio_ring_queued(const struct audio_ring *r);
uint32_t audio_ring_free(const struct audio_ring *r);
/* Drop everything queued. Only when producer and consumer are both quiet
 * (close, reset): it moves the consumer's index. */
void audio_ring_flush(struct audio_ring *r);
