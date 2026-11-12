/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). See include/audio_stall.h. */
#include "audio_stall.h"
void audio_stall_reset(struct audio_stall *s) { s->armed = 0; s->last = 0; s->since_us = 0; }
int audio_stall_check(struct audio_stall *s, uint32_t now_us, uint32_t completions, uint32_t limit_us) {
    if (!s->armed || completions != s->last) { s->armed = 1; s->last = completions; s->since_us = now_us; return 0; }
    if (now_us - s->since_us < limit_us) return 0;
    s->since_us = now_us; /* report once per limit period; the caller falls back at the first */
    return 1;
}
static void put(char *o, size_t cap, size_t *n, const char *s) { while (*s && *n + 1 < cap) o[(*n)++] = *s++; o[*n] = 0; }
static void num(char *o, size_t cap, size_t *n, const char *k, uint32_t v) {
    char t[11]; unsigned c = 0; put(o, cap, n, k);
    do { t[c++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (c) { if (*n + 1 < cap) o[(*n)++] = t[--c]; else c--; }
    o[*n] = 0;
}
void audio_stall_line(char *out, size_t cap, const struct audio_stall_info *i) {
    size_t n = 0; if (!cap) return; out[0] = 0;
    put(out, cap, &n, "audio_stall");
    num(out, cap, &n, " ring_queued=", i->ring_queued); num(out, cap, &n, " ring_free=", i->ring_free);
    num(out, cap, &n, " dma_seg=", i->dma_conblk_idx); num(out, cap, &n, " completions=", i->completions);
    put(out, cap, &n, " pixel_source="); put(out, cap, &n, i->pixel_src && *i->pixel_src ? i->pixel_src : "none");
    num(out, cap, &n, " pixel_used_hz=", i->pixel_used_hz); num(out, cap, &n, " N=", i->n); num(out, cap, &n, " CTS=", i->cts);
    put(out, cap, &n, "; falling back to the null drain");
}
