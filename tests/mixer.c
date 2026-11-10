/* SPDX-License-Identifier: MIT; native checks of core/mixer.c: tone frequency
 * by zero crossings, duration, tune ticks, two-voice mix with clipping, idle
 * silence, and no ring overrun when pumped against the null backend. */
#include "clock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_null.h"
#include "mixer.h"
uint64_t clock_us(void) { return 0; }
static uint32_t buf[48000];
static int16_t L(uint32_t w) { return (int16_t)(w & 0xFFFF); }
static uint32_t crossings(const uint32_t *p, uint32_t n) { /* rising edges */
    uint32_t c = 0; for (uint32_t i = 1; i < n; i++) if (L(p[i - 1]) < 0 && L(p[i]) > 0) c++; return c;
}
static void frequency(void) {
    static const uint32_t hz[] = {110, 440, 1000, 4000, 12000};
    for (unsigned k = 0; k < sizeof hz / sizeof *hz; k++) {
        struct mixer m; mixer_init(&m);
        assert(mixer_tone(&m, hz[k], 0, MIXER_VOL_ONE) == 0);
        mixer_render(&m, buf, 48000);
        uint32_t c = crossings(buf, 48000);
        assert(c + 1 >= hz[k] && c <= hz[k] + 1); /* within one cycle in 1 s */
        for (int i = 0; i < 48000; i++) assert(buf[i] >> 16 == (buf[i] & 0xFFFF)); /* L == R */
    }
}
static void duration(void) {
    struct mixer m; mixer_init(&m);
    mixer_tone(&m, 440, 100, MIXER_VOL_ONE); /* 4800 frames */
    mixer_render(&m, buf, 4799); assert(mixer_active(&m) == 1);
    mixer_render(&m, buf, 1); assert(mixer_active(&m) == 0);
    mixer_render(&m, buf, 100);
    for (int i = 0; i < 100; i++) assert(buf[i] == 0);
    assert(mixer_tone(&m, 10, 10, 256) == -1); /* inaudible */
}
static void tune(void) {
    static uint16_t t[SND_STEPS]; /* 20 ticks of 1000 Hz, 5 silent, 11 of 2000 */
    for (int i = 0; i < 20; i++) t[i] = 1000;
    for (int i = 25; i < SND_STEPS; i++) t[i] = 2000;
    struct mixer m; mixer_init(&m);
    mixer_tune(&m, t, SND_STEPS, MIXER_VOL_ONE);
    mixer_render(&m, buf, 20000);
    /* total length 36 ticks = 36*8191*48000/1193180 = 11862.2 frames */
    assert(mixer_active(&m) == 0);
    uint32_t last = 0; for (uint32_t i = 0; i < 20000; i++) if (buf[i]) last = i;
    assert(last > 11700 && last < 11863);
    /* tick 0..19 span 20 ticks = 6590.6 frames: 1000 Hz = 137.3 cycles */
    uint32_t c = crossings(buf, 6590); assert(c >= 136 && c <= 138);
    for (uint32_t i = 6600; i < 8230; i++) assert(buf[i] == 0); /* silent ticks 20..24 */
    uint32_t c2 = crossings(buf + 8300, 3500); /* 2000 Hz over 3500 frames = 145.8 */
    assert(c2 >= 144 && c2 <= 147);
    int16_t peak = 0; for (int i = 0; i < 6000; i++) if (L(buf[i]) > peak) peak = L(buf[i]);
    assert(peak == (int16_t)((32767u * 31u) >> 8) || peak > 3900); /* speaker gain 0.12 */
}
static void mix_clip(void) {
    struct mixer m; mixer_init(&m);
    mixer_tone(&m, 440, 0, MIXER_VOL_ONE); mixer_tone(&m, 440, 0, MIXER_VOL_ONE); /* in phase: 2x */
    mixer_render(&m, buf, 4800);
    int16_t hi = 0, lo = 0; for (int i = 0; i < 4800; i++) { int16_t s = L(buf[i]); if (s > hi) hi = s; if (s < lo) lo = s; }
    assert(hi == 32767 && lo == -32768);
    /* two half-volume voices sum to one full voice exactly */
    struct mixer a, b; mixer_init(&a); mixer_init(&b);
    mixer_tone(&a, 700, 0, 128); mixer_tone(&a, 700, 0, 128); mixer_tone(&b, 700, 0, 256);
    static uint32_t o1[4800], o2[4800]; mixer_render(&a, o1, 4800); mixer_render(&b, o2, 4800);
    for (int i = 0; i < 4800; i++) assert(abs(L(o1[i]) - L(o2[i])) <= 1);
    mixer_set_volume(&b, 0, 0); mixer_render(&b, o2, 100);
    for (int i = 0; i < 100; i++) assert(o2[i] == 0);
    /* voice limit */
    mixer_init(&m); for (int i = 0; i < MIXER_VOICES; i++) assert(mixer_tone(&m, 300 + i, 0, 256) == i);
    assert(mixer_tone(&m, 300, 0, 256) == -1);
    mixer_stop(&m, 3); assert(mixer_tone(&m, 500, 0, 256) == 3);
}
static void idle(void) {
    struct mixer m; mixer_init(&m); memset(buf, 0xAA, sizeof buf);
    mixer_render(&m, buf, 1000);
    for (int i = 0; i < 1000; i++) assert(buf[i] == 0);
}
/* Independent 64-bit amplitude oracle, with clipping only after scaling. */
static uint32_t volume_sample(uint32_t volume, int tune_voice, unsigned sample) {
    int64_t amplitude = INT64_C(32767) * volume;
    amplitude = tune_voice ? amplitude * 31 / 65536 : amplitude / 256;
    /* At 12000 Hz the phase step is just under a quarter cycle. */
    int64_t value = sample % 4 < 2 ? amplitude : -amplitude;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    uint32_t packed = (uint16_t)value;
    return packed | (packed << 16);
}
static void all_volumes(void) {
    static const uint16_t freq = 12000;
    for (int tune_voice = 0; tune_voice <= 1; tune_voice++) {
        for (uint32_t volume = 0; volume <= UINT16_MAX; volume++) {
            struct mixer m; mixer_init(&m);
            int voice = tune_voice ? mixer_tune(&m, &freq, 1, (uint16_t)volume)
                                   : mixer_tone(&m, freq, 0, (uint16_t)volume);
            assert(voice == 0);
            mixer_render(&m, buf, 8);
            for (unsigned i = 0; i < 8; i++)
                assert(buf[i] == volume_sample(volume, tune_voice, i));
            uint32_t updated = UINT16_MAX - volume;
            mixer_set_volume(&m, voice, (uint16_t)updated);
            mixer_render(&m, buf, 8);
            for (unsigned i = 0; i < 8; i++)
                assert(buf[i] == volume_sample(updated, tune_voice, i + 8));
        }
    }
}
static void pump_null(void) {
    audio_dev_init(); assert(audio_dev_open() == 0);
    struct mixer m; mixer_init(&m); mixer_tone(&m, 440, 0, 256);
    uint64_t t = 0; uint32_t total = 0;
    audio_null_tick(t); /* start the clock */
    for (int ms = 0; ms < 2000; ms++) { /* 1 ms ticks, pump every tick with a small ask */
        total += mixer_pump_dev(&m, 200);
        t += 1000; audio_null_tick(t);
    }
    assert(audio0.ring.overruns == 0 && audio0.ring.overrun_frames == 0);
    assert(audio0.ring.underruns == 0);
    assert(total == 96000 + audio_ring_queued(&audio0.ring) && audio_ring_queued(&audio0.ring) <= AUDIO_RING_FRAMES);
    /* a greedy pump fills the ring but never overruns it */
    mixer_pump_dev(&m, 100000); assert(audio_ring_free(&audio0.ring) == 0);
    assert(mixer_pump_dev(&m, 100) == 0 && audio0.ring.overruns == 0);
    audio_dev_close();
}
int main(void) { frequency(); duration(); tune(); mix_clip(); idle(); all_volumes(); pump_null(); puts("mixer ok"); return 0; }
