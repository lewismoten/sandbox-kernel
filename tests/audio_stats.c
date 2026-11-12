/* SPDX-License-Identifier: MIT; host checks of core/audio_stats.c and audio_diag_stat. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_stats.h"
#include "audio_diag.h"
#include "hdmi_audio.h"
int main(void) {
    struct audio_stats s; const uint32_t ring = 12288;
    uint32_t t = 4000000000u; /* across the 32-bit wrap */
    audio_stats_begin(&s, t, 100);
    /* 48 kHz stereo = 384 bytes per ms; looks every 1 ms for 5 s, the ring position wrapping. */
    uint32_t pos = 0;
    for (unsigned i = 0; i < 5000; i++) {
        t += 1000; pos = (pos + 384) % ring;
        audio_stats_sample(&s, t, pos, ring, i == 7 ? MAI_CTL_ERRORE | MAI_CTL_EMPTY : MAI_CTL_FULL, i == 9 ? 2u : 0, 0);
    }
    assert(audio_stats_due(&s, t));
    struct audio_stats_ctx c = {725, 1, 16384, 3, 0x1234, 0x1, 2};
    char a[192], b[192]; audio_stats_lines(&s, t, &c, a, sizeof a, b, sizeof b);
    assert(strstr(a, "stat rate dt_ms=5000 bytes_s=383923 ")); /* the first look only sets the base */
    assert(strstr(a, "frames_s=47990 segs=625 dma_detect=poll fg_gap_max_ms=1 gaps_over_32ms=0 pos_bad=0 stall_silence=2"));
    assert(strstr(b, "stat flags looks=5000 empty=1 full=4999 busy=0 underflow=1 overflow=0 dlate=0 dbg_fifo=1"));
    assert(strstr(b, "cs=0x0001 mai=0x1234 ring_queued=16384 ring_underruns=3"));
    assert(!audio_stats_due(&s, t)); /* window restarted */
    /* A stall in the foreground, and a position outside the ring. */
    t += 100000; audio_stats_sample(&s, t, 0, ring, 0, 0, 0x100u);
    t += 1000; audio_stats_sample(&s, t, 99999, ring, 0, 0, 0);
    audio_stats_lines(&s, t + 4900000u, &c, a, sizeof a, b, sizeof b);
    assert(strstr(a, "fg_gap_max_ms=100 gaps_over_32ms=1 pos_bad=1") && strstr(b, "cs_err=1"));
    /* Stat lines are retired before boot lines when the log is full. */
    static struct audio_diag d; memset(&d, 0, sizeof d);
    audio_diag_line(&d, 1, "backend=hdmi reason=ok attempt=1");
    for (unsigned i = 0; i < 200; i++) { audio_diag_stat(&d, 5000 + i, a); audio_diag_stat(&d, 5000 + i, b); }
    assert(!d.dropped && strstr(d.text, "1 backend=hdmi reason=ok attempt=1\n") == d.text);
    assert(d.len <= AUDIO_LOG_BYTES && strstr(d.text, "stat flags"));
    puts("audio stats ok"); return 0;
}
