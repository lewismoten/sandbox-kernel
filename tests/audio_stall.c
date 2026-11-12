/* SPDX-License-Identifier: MIT; host checks of core/audio_stall.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_stall.h"
int main(void) {
    struct audio_stall s; audio_stall_reset(&s);
    uint32_t t = 4000000000u; /* near the 32-bit wrap */
    assert(!audio_stall_check(&s, t, 10, AUDIO_STALL_US));
    t += 8000; assert(!audio_stall_check(&s, t, 11, AUDIO_STALL_US));   /* progress */
    t += 200000; assert(!audio_stall_check(&s, t, 11, AUDIO_STALL_US)); /* quiet, not yet */
    t += 60000; assert(audio_stall_check(&s, t, 11, AUDIO_STALL_US));   /* stalled, across the wrap */
    t += 10000; assert(!audio_stall_check(&s, t, 11, AUDIO_STALL_US));  /* reported once per period */
    assert(!audio_stall_check(&s, t + 1, 12, AUDIO_STALL_US));          /* progress clears it */
    char l[192]; struct audio_stall_info i = { 16384, 0, 3, 41, 148500000, 6144, 148500, "mode-table" };
    audio_stall_line(l, sizeof l, &i);
    assert(strstr(l, "audio_stall ring_queued=16384 ring_free=0 dma_seg=3 completions=41 pixel_source=mode-table pixel_used_hz=148500000 N=6144 CTS=148500"));
    char tiny[8]; audio_stall_line(tiny, sizeof tiny, &i); assert(strlen(tiny) == 7);
    puts("audio stall ok"); return 0;
}
