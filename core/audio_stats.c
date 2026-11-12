/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). See include/audio_stats.h. */
#include <string.h>
#include "audio_stats.h"
#include "hdmi_audio.h"

void audio_stats_begin(struct audio_stats *s, uint32_t now_us, uint32_t comp) {
    memset(s, 0, sizeof *s);
    s->t0_us = s->last_us = now_us; s->comp0 = comp; s->started = 1;
}
void audio_stats_sample(struct audio_stats *s, uint32_t now_us, uint32_t pos, uint32_t ring_bytes,
                        uint32_t mai, uint32_t dbg, uint32_t cs) {
    uint32_t gap = now_us - s->last_us; s->last_us = now_us;
    if (gap > s->max_gap_us) s->max_gap_us = gap;
    /* More than half a ring (32 ms) between looks can hide a lap: the byte count is then a floor. */
    if (gap > 32000u) s->big_gaps++;
    if (pos < ring_bytes) {
        if (s->pos_ok) s->bytes += (pos + ring_bytes - s->pos_last) % ring_bytes;
        s->pos_last = pos; s->pos_ok = 1;
    } else s->pos_bad++;
    s->samples++;
    if (mai & MAI_CTL_EMPTY) s->empty++;
    if (mai & MAI_CTL_FULL) s->full++;
    if (mai & MAI_CTL_BUSY) s->busy++;
    if (mai & MAI_CTL_ERRORE) s->under++;
    if (mai & MAI_CTL_ERRORF) s->over++;
    if (mai & MAI_CTL_DLATE) s->late++;
    if (dbg & 1u) s->dbg_last++;
    if (dbg & 2u) s->dbg_fifo++;
    if (dbg & 4u) s->dbg_read++;
    if (cs & 0x100u) s->cs_err++;
}
int audio_stats_due(const struct audio_stats *s, uint32_t now_us) {
    return s->started && now_us - s->t0_us >= AUDIO_STATS_WINDOW_US;
}
static void put(char **p, char *end, const char *t) { while (*t && *p < end) *(*p)++ = *t++; }
static void num(char **p, char *end, const char *k, uint32_t v) {
    char t[12]; int n = 0; put(p, end, k);
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n && *p < end) *(*p)++ = t[--n];
}
static void hx(char **p, char *end, const char *k, uint32_t v) {
    put(p, end, k);
    for (int i = 3; i >= 0; i--) if (*p < end) *(*p)++ = "0123456789abcdef"[(v >> (4 * i)) & 15u];
}
size_t audio_stats_lines(struct audio_stats *s, uint32_t now_us, const struct audio_stats_ctx *c,
                         char *o1, size_t cap1, char *o2, size_t cap2) {
    uint32_t dt = now_us - s->t0_us, ms = dt / 1000u ? dt / 1000u : 1u;
    uint32_t bps = s->bytes / ms * 1000u + s->bytes % ms * 1000u / ms; /* 32-bit only: the kernel has no 64-bit divide */
    char *p = o1, *e = o1 + cap1 - 1;
    put(&p, e, "stat rate"); num(&p, e, " dt_ms=", ms); num(&p, e, " bytes_s=", bps);
    put(&p, e, " (48 kHz stereo=384000)"); num(&p, e, " frames_s=", bps / 8u);
    num(&p, e, " segs=", c->comp - s->comp0); put(&p, e, c->poll_mode ? " dma_detect=poll" : " dma_detect=irq");
    num(&p, e, " fg_gap_max_ms=", s->max_gap_us / 1000u); num(&p, e, " gaps_over_32ms=", s->big_gaps);
    num(&p, e, " pos_bad=", s->pos_bad); *p = 0; size_t n1 = (size_t)(p - o1);
    p = o2; e = o2 + cap2 - 1;
    put(&p, e, "stat flags"); num(&p, e, " looks=", s->samples);
    num(&p, e, " empty=", s->empty); num(&p, e, " full=", s->full); num(&p, e, " busy=", s->busy);
    num(&p, e, " underflow=", s->under); num(&p, e, " overflow=", s->over); num(&p, e, " dlate=", s->late);
    num(&p, e, " dbg_fifo=", s->dbg_fifo); num(&p, e, " dbg_read=", s->dbg_read);
    num(&p, e, " dbg_last=", s->dbg_last); num(&p, e, " cs_err=", s->cs_err);
    hx(&p, e, " cs=0x", c->dma_cs); hx(&p, e, " mai=0x", c->mai_now);
    num(&p, e, " ring_queued=", c->ring_queued); num(&p, e, " ring_underruns=", c->ring_underruns);
    *p = 0;
    audio_stats_begin(s, now_us, c->comp);
    return n1;
}
