/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Periodic HDMI audio health numbers for
 * AUDIO.TXT: how many bytes the DMA really consumed per second (the MAI strobe
 * paces it through DREQ 17, so 48 kHz stereo is 384000 bytes/s of 32-bit
 * subframes), the MAI FIFO flags and the DMA error bits. Pure logic, host-tested;
 * drivers/hdmi_audio.c does the register reads and clears the sticky flags. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define AUDIO_STATS_WINDOW_US 5000000u
struct audio_stats {
    uint32_t t0_us, last_us, max_gap_us, big_gaps;
    uint32_t pos_last, bytes, pos_ok, pos_bad;
    uint32_t samples, empty, full, busy;
    uint32_t under, over, late;           /* samples that saw the sticky MAI flag set */
    uint32_t dbg_last, dbg_fifo, dbg_read, cs_err;
    uint32_t comp0;
    int started;
};
/* Start a window at now_us; comp is the running completion count. */
void audio_stats_begin(struct audio_stats *s, uint32_t now_us, uint32_t comp);
/* One observation. pos = DMA source address minus the ring's bus base, valid
 * when below ring_bytes. mai = MAI_CTL read, dbg = DMA DEBUG, cs = DMA CS. */
void audio_stats_sample(struct audio_stats *s, uint32_t now_us, uint32_t pos, uint32_t ring_bytes,
                        uint32_t mai, uint32_t dbg, uint32_t cs);
int audio_stats_due(const struct audio_stats *s, uint32_t now_us);
struct audio_stats_ctx { uint32_t comp, poll_mode, ring_queued, ring_underruns, mai_now, dma_cs, stall_silence; };
/* Two lines (each under 190 bytes), then the window restarts. Line 1 starts
 * "stat rate", line 2 "stat flags". Returns line 1's length; line 2 goes to out2. */
size_t audio_stats_lines(struct audio_stats *s, uint32_t now_us, const struct audio_stats_ctx *c,
                         char *out1, size_t cap1, char *out2, size_t cap2);
