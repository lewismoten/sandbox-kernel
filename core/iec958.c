/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). See include/iec958.h for sources. */
#include "iec958.h"
#include <string.h>
int iec958_status(uint32_t rate, int right, uint8_t out[24]) {
    unsigned fs;
    if (rate == 48000) fs = 0x2; else if (rate == 44100) fs = 0x0; else if (rate == 32000) fs = 0x3; else return -1; /* VERIFY: IEC 60958-3 byte 3 bits 0-3 */
    memset(out, 0, 24);
    out[0] = 0x04; /* consumer (bit0 0), PCM (bit1 0), bit2 copyright not asserted, no pre-emphasis, mode 0 */
    out[1] = 0x00; /* category: general */
    out[2] = (uint8_t)((right ? 2u : 1u) << 4); /* source 0; channel number 1 or 2 */
    out[3] = (uint8_t)fs; /* sampling frequency; clock accuracy level II (bits 4-5 = 0) */
    out[4] = 0x04; /* max word length 20 bits (bit0 0), 16 bits used (bits 1-3 = 010). VERIFY */
    return 0;
}
uint32_t iec958_parity(uint32_t w) {
    w &= 0xfffffff0u;
    w ^= w >> 16; w ^= w >> 8; w ^= w >> 4; w ^= w >> 2; w ^= w >> 1;
    return w & 1u;
}
uint32_t iec958_subframe(unsigned pre, int16_t s, unsigned cbit) {
    uint32_t w = (pre & 0xfu) | ((uint32_t)(uint16_t)s << 12) | ((cbit & 1u) << 30);
    return w | (iec958_parity(w) << 31);
}
unsigned iec958_pack(const uint32_t *pcm, uint32_t frames, unsigned first,
                     const uint8_t sL[24], const uint8_t sR[24], uint32_t *out) {
    unsigned n = first % IEC958_BLOCK_FRAMES;
    for (uint32_t i = 0; i < frames; i++) {
        out[2 * i] = iec958_subframe(n == 0 ? IEC958_PRE_B : IEC958_PRE_M, (int16_t)(pcm[i] & 0xffffu), iec958_status_bit(sL, n));
        out[2 * i + 1] = iec958_subframe(IEC958_PRE_W, (int16_t)(pcm[i] >> 16), iec958_status_bit(sR, n));
        if (++n == IEC958_BLOCK_FRAMES) n = 0;
    }
    return n;
}
/* N is the recommended 128*fs/1000 (HDMI 1.4b table 7-1 to 7-3: 4096, 6272, 6144 for 32, 44.1, 48 kHz at
 * these clocks); CTS then follows from 128*fs = f_pix*N/CTS. 25.2 MHz/1.001 (the 25.175 MHz VGA clock) uses the
 * spec's 44.1 kHz pair 7007/31250. Each row was recomputed from that identity; VERIFY against the spec tables. */
static const struct {uint32_t fs, pix, n, cts;} nct[] = {
    {48000, 25200000, 6144, 25200}, {48000, 25175000, 6144, 25175}, {48000, 27000000, 6144, 27000},
    {48000, 74250000, 6144, 74250}, {48000, 148500000, 6144, 148500},
    {44100, 25200000, 6272, 28000}, {44100, 25175000, 7007, 31250}, {44100, 27000000, 6272, 30000},
    {44100, 74250000, 6272, 82500}, {44100, 148500000, 6272, 165000},
    {32000, 25200000, 4096, 25200}, {32000, 25175000, 4096, 25175}, {32000, 27000000, 4096, 27000},
    {32000, 74250000, 4096, 74250}, {32000, 148500000, 4096, 148500},
};
int hdmi_nct(uint32_t rate, uint32_t pixel_hz, uint32_t *n, uint32_t *cts) {
    for (size_t i = 0; i < sizeof nct / sizeof *nct; i++)
        if (nct[i].fs == rate && nct[i].pix == pixel_hz) { *n = nct[i].n; *cts = nct[i].cts; return 0; }
    return -1;
}
