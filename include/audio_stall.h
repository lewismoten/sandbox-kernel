/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Detects an HDMI audio DMA that has stopped
 * completing segments, and words the AUDIO.TXT line for it. Pure logic, host-tested. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define AUDIO_STALL_US 250000u /* no segment completed for this long (a segment is 8 ms) */
struct audio_stall { uint32_t last, since_us; int armed; };
struct audio_stall_info {
    uint32_t ring_queued, ring_free, dma_conblk_idx, completions, pixel_used_hz, n, cts;
    const char *pixel_src;
};
void audio_stall_reset(struct audio_stall *s);
/* Call with the IRQ's running completion count. Returns 1 once, when the count
 * has not moved for limit_us (wrap-safe); it then stays 0 until reset or progress. */
int audio_stall_check(struct audio_stall *s, uint32_t now_us, uint32_t completions, uint32_t limit_us);
/* "audio_stall ring_queued=.. ring_free=.. dma_seg=.. completions=.. pixel_source=.. pixel_used_hz=.. N=.. CTS=..; null drain" */
void audio_stall_line(char *out, size_t cap, const struct audio_stall_info *i);
