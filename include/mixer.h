/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Fixed-point mixer: MIXER_VOICES voices
 * summed into the /dev/audio ring's stereo S16 frames at AUDIO_RING_RATE.
 * Integer math only (no floats, no 64-bit divide) in the render path.
 * Kernel-internal for now: no app-facing ABI. See docs/audio-device.md. */
#pragma once
#include <stdint.h>
#include "audio_ring.h"
#define MIXER_VOICES 8
#define MIXER_VOL_ONE 256u /* Q8: 256 = full scale; all uint16_t volumes supported */
/* PC speaker tune: Game-Maker .SND effects hold SND_STEPS (36) frequencies in
 * Hz, one per timer tick of 1193180/8191 Hz (about 145.67 per second). A
 * frequency of 0 (or under 20 Hz, inaudible) is silence; above 20000 clamps.
 * Sources: web/packages/engine/src/formats/sound.ts, audio/engine.ts
 * (playSpeaker), audio/cmfPlayer.ts (TICK_RATE) in Game-Maker Sandbox Edition. */
#define SND_STEPS 36
#define SPEAKER_MIN_HZ 20u
#define SPEAKER_MAX_HZ 20000u
#define SPEAKER_TICK_NUM 1193180u /* PIT clock */
#define SPEAKER_TICK_DIV 8191u    /* floor(65535 / 8) */
#define SPEAKER_GAIN 31u          /* 0.12 in Q8, the web engine's speaker gain */
struct mixer_voice {
    uint8_t active;
    uint16_t vol;            /* Q8 */
    uint32_t phase, step;    /* 32-bit phase accumulator; step = hz * 2^32 / rate */
    const uint16_t *freqs;   /* tune table (caller keeps it alive), or 0 for a plain tone */
    uint32_t nfreqs, tick;   /* tune length; ticks fully played */
    uint64_t frames;         /* frames rendered since start (tune clock) */
    uint64_t remaining;      /* frames left for a plain tone; ignored for tunes */
};
struct mixer { struct mixer_voice v[MIXER_VOICES]; };
void mixer_init(struct mixer *m);
/* Start a square tone of hz for `ms` milliseconds (0 = until stopped). Returns
 * the voice index, or -1 when all voices are busy or hz is out of range. */
int mixer_tone(struct mixer *m, uint32_t hz, uint32_t ms, uint16_t vol);
/* Start a tune: n frequencies in Hz, one per speaker tick. The table must outlive it. */
int mixer_tune(struct mixer *m, const uint16_t *freqs, uint32_t n, uint16_t vol);
void mixer_stop(struct mixer *m, int voice);
void mixer_set_volume(struct mixer *m, int voice, uint16_t vol);
uint32_t mixer_active(const struct mixer *m);
/* Add every active voice into `frames` stereo frames (each word = L low, R high),
 * with saturation. Writes silence when nothing plays. */
void mixer_render(struct mixer *m, uint32_t *out, uint32_t frames);
/* Render what fits in the ring's free space (never more than `max` frames, so
 * the null backend's drain can pace it); returns frames queued. No overrun. */
uint32_t mixer_pump(struct mixer *m, struct audio_ring *r, uint32_t max);
/* Same, into /dev/audio's own ring (audio_dev_write), so the device counts the
 * stream as started and the backend's underrun rules apply. */
uint32_t mixer_pump_dev(struct mixer *m, uint32_t max);
