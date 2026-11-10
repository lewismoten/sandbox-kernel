/* SPDX-License-Identifier: MIT; host checks of core/iec958.c: parity, channel status over a block, packing, N/CTS. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "iec958.h"
static unsigned pop(uint32_t v) { unsigned c = 0; for (; v; v &= v - 1) c++; return c; }
int main(void) {
    assert(IEC958_PRE_B == 8 && IEC958_PRE_M == 2 && IEC958_PRE_W == 4);
    assert(iec958_subframe(IEC958_PRE_B, 0x1234, 0) == 0x81234008u);
    assert(iec958_subframe(IEC958_PRE_M, -32768, 1) == 0x48000002u);
    assert(iec958_subframe(IEC958_PRE_W, 32767, 0) == 0x87fff004u);
    for (uint32_t v = 0; v < 65536; v += 7) { /* every word has even parity over bits 4-31 */
        uint32_t w = iec958_subframe(IEC958_PRE_M, (int16_t)v, v & 1);
        assert(pop(w >> 4) % 2 == 0);
        assert(((w >> 12) & 0xffffu) == v && (w & 0xf) == IEC958_PRE_M && ((w >> 30) & 1) == (v & 1));
    }
    assert(iec958_subframe(IEC958_PRE_W, 0, 0) == (IEC958_PRE_W)); /* all-zero payload: parity 0 */
    assert((iec958_subframe(IEC958_PRE_W, 1, 0) >> 31) == 1);      /* one set bit: parity bit set */
    uint8_t l[24], r[24], l44[24];
    assert(iec958_status(48000, 0, l) == 0 && iec958_status(48000, 1, r) == 0 && iec958_status(44100, 0, l44) == 0);
    assert(iec958_status(22050, 0, l) == -1);
    assert(iec958_status(48000, 0, l) == 0);
    assert((l[3] & 0xf) == 0x2 && (l44[3] & 0xf) == 0x0 && (l[0] & 3) == 0 && l[2] >> 4 == 1 && r[2] >> 4 == 2);
    assert(l[4] == 0x02 && r[4] == 0x02); /* 16-bit samples, max word length 20 (VERIFY on a TV) */
    for (unsigned i = 5; i < 24; i++) assert(l[i] == 0 && r[i] == 0);
    unsigned ones = 0; for (unsigned n = 0; n < 192; n++) ones += iec958_status_bit(l, n);
    assert(ones == pop(l[0]) + pop(l[2]) + pop(l[3]) + pop(l[4]));
    /* pack 400 frames across two block boundaries; decode and compare */
    static uint32_t pcm[400], out[800];
    for (unsigned i = 0; i < 400; i++) pcm[i] = (uint16_t)(i * 37 - 5000) | ((uint32_t)(uint16_t)(-(int)i * 11) << 16);
    unsigned n = iec958_pack(pcm, 400, 100, l, r, out);
    assert(n == (100 + 400) % 192);
    unsigned f = 100;
    uint8_t gotL[24] = {0}, gotR[24] = {0};
    for (unsigned i = 0; i < 400; i++, f = (f + 1) % 192) {
        uint32_t a = out[2 * i], b = out[2 * i + 1];
        assert((a & 0xf) == (f == 0 ? IEC958_PRE_B : IEC958_PRE_M) && (b & 0xf) == IEC958_PRE_W);
        assert(pop(a >> 4) % 2 == 0 && pop(b >> 4) % 2 == 0);
        assert((int16_t)(a >> 12) == (int16_t)(pcm[i] & 0xffff) && (int16_t)(b >> 12) == (int16_t)(pcm[i] >> 16));
        assert(((a >> 30) & 1) == iec958_status_bit(l, f) && ((b >> 30) & 1) == iec958_status_bit(r, f));
        if (i >= 92 && i < 92 + 192) { /* a full block (frames 0..191) rebuilt from the C bits */
            if ((a >> 30) & 1) gotL[f >> 3] |= 1u << (f & 7);
            if ((b >> 30) & 1) gotR[f >> 3] |= 1u << (f & 7);
        }
    }
    assert(!memcmp(gotL, l, 24) && !memcmp(gotR, r, 24));
    uint32_t N, C;
    struct {uint32_t fs, pix, n, c;} k[] = {{48000,148500000,6144,148500},{48000,74250000,6144,74250},{48000,25200000,6144,25200},
        {44100,148500000,6272,165000},{44100,74250000,6272,82500},{44100,25175000,7007,31250},{32000,74250000,4096,74250}};
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) { assert(hdmi_nct(k[i].fs, k[i].pix, &N, &C) == 0 && N == k[i].n && C == k[i].c); }
    static const uint32_t rates[] = {32000, 44100, 48000}, clk[] = {25200000, 25175000, 27000000, 74250000, 148500000};
    for (unsigned a = 0; a < 3; a++) for (unsigned b = 0; b < 5; b++) {
        assert(hdmi_nct(rates[a], clk[b], &N, &C) == 0);
        double want = 128.0 * rates[a], have = (double)clk[b] * N / C;
        assert((have - want) / want < 1e-4 && (want - have) / want < 1e-4);
    }
    assert(hdmi_nct(96000, 148500000, &N, &C) == -1);
    puts("iec958 ok");
    return 0;
}
