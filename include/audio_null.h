/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). /dev/audio device state and the null
 * backend: it "plays" by draining the ring at exactly AUDIO_RING_RATE frames
 * a second, driven by timestamps from the timer tick. No hardware. A real
 * backend (PWM, HDMI) replaces audio_null_tick with a DMA refill. */
#pragma once
#include <stdint.h>
#include "audio_ring.h"
struct audio_dev { struct audio_ring ring; uint32_t open; /* 0 or 1: one writer */ uint64_t last_us, frac; int clocked, wrote; /* wrote: first write seen since open */ };
extern struct audio_dev audio0;
void audio_dev_init(void);
uint32_t audio_dev_write(const void *src, uint32_t bytes); /* whole frames queued; marks the stream started */
int audio_dev_open(void);          /* 0, or -16 (EBUSY) while another writer holds it */
void audio_dev_close(void);        /* flushes the ring: queued audio is dropped, not played */
/* Drain what is due since the last tick. Does nothing (and counts no
 * underrun) while nobody has the device open, so silence between games is free. */
void audio_null_tick(uint64_t now_us);
void audio_null_task(void);        /* main-loop hook: audio_null_tick(clock_us()) */

/* A hardware backend (HDMI) installs these; all null means the null backend runs.
 * audio_null_task calls task instead of draining when it is set. */
struct audio_hw { void (*task)(void); int (*tone)(uint32_t hz, uint32_t ms); void (*report)(char *out, size_t cap); };
extern struct audio_hw audio_hw;
/* USB test commands. `line` is the command without its newline. Returns 0 when it
 * is not an audio command; else 1 and fills `reply` (one line, with "\n"):
 *   TONE <hz> <ms>   quiet test tone through the mixer (50..4000 Hz, 1..1000 ms)
 *   AUDIO            backend state and counters */
int audio_command(const char *line, char *reply, size_t cap);
