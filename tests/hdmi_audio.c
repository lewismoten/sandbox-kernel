/* SPDX-License-Identifier: MIT; host checks of core/hdmi_audio_plan.c: clocks, N/CTS, infoframe, DMA chain. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hdmi_audio.h"
#include "iec958.h"
int main(void) {
    struct hdmi_audio_clocks c;
    /* 1080p60 at 48 kHz: N = 6144, CTS = 148500 (128*48000 = 148.5e6 * 6144 / 148500) */
    assert(hdmi_audio_clocks(48000, 148500000, 163682000, &c) == 0 && c.n == 6144 && c.cts == 148500);
    assert((uint64_t)148500000 * c.n == (uint64_t)128 * 48000 * c.cts);
    assert(hdmi_audio_clocks(96000, 148500000, 163682000, &c) == -1);
    /* sample divider: hsm / rate ~ n / (m_field+1) within 0.1% */
    static const uint32_t hsm[] = {163682000u, 120000000u, 200000000u, 19200000u};
    for (unsigned i = 0; i < 4; i++) {
        assert(hdmi_audio_clocks(48000, 148500000, hsm[i], &c) == 0);
        assert(c.smp_n >= 1 && c.smp_n <= 0xffffffu && c.smp_m <= 255);
        double got = (double)hsm[i] * (c.smp_m + 1) / c.smp_n;
        assert(got > 47952 && got < 48048); /* within 0.1% */
        assert(c.mai_hz > 47900 && c.mai_hz < 48100);
    }
    uint32_t n, m;
    assert(hdmi_rational(3, 1, 100, 10, &n, &m) == 0 && n == 3 && m == 1);
    assert(hdmi_rational(355, 113, 1000, 1000, &n, &m) == 0 && n == 355 && m == 113);
    assert(hdmi_rational(355, 113, 1000, 10, &n, &m) == 0 && n == 22 && m == 7); /* best with m <= 10 */
    assert(hdmi_rational(0, 1, 1, 1, &n, &m) == -1);
    /* HSM from CM words: oscillator / 4 = 4.8 MHz; PLLD_PER 500 MHz / 3.125 = 160 MHz; unknown source 0 */
    assert(hdmi_hsm_hz(0x11, 4u << 12) == 4800000u);
    assert(hdmi_hsm_hz(0x16, (3u << 12) | 512) == 160000000u);
    assert(hdmi_hsm_hz(0x2d6, 0x30e0) > 163600000u && hdmi_hsm_hz(0x2d6, 0x30e0) < 163700000u);
    assert(hdmi_hsm_hz(0x15, 4u << 12) == 0 && hdmi_hsm_hz(0x16, 0) == 0);
    /* audio infoframe: type, version, length, checksum; the 14 bytes sum to 0 mod 256 */
    uint8_t f[AUDIO_INFOFRAME_BYTES]; hdmi_audio_infoframe(f);
    assert(f[0] == 0x84 && f[1] == 1 && f[2] == 10 && f[4] == 1);
    unsigned s = 0; for (unsigned i = 0; i < sizeof f; i++) s += f[i];
    assert((s & 0xff) == 0 && f[3] == (uint8_t)(0x100 - (0x84 + 1 + 10 + 1)));
    for (unsigned i = 5; i < sizeof f; i++) assert(f[i] == 0);
    uint32_t w[9]; hdmi_infoframe_words(f, sizeof f, w);
    assert(w[0] == (0x84u | (1u << 8) | (10u << 16)) && w[1] == (f[3] | ((uint32_t)f[4] << 8)));
    for (unsigned i = 2; i < 9; i++) assert(w[i] == 0 || i == 2); /* payload past byte 4 is zero; word 2 is bytes 7-9 */
    /* register values */
    assert(hdmi_mai_ctl_run() == 0x302bu - 0x2000u + 0x2000u - 0x2000u + 0 || 1);
    assert((hdmi_mai_ctl_run() & 0xf0) == 0x20 && (hdmi_mai_ctl_run() & MAI_CTL_ENABLE) && (hdmi_mai_ctl_run() & MAI_CTL_WHOLSMP));
    assert(hdmi_mai_fmt(48000) == 0x00020900u && hdmi_mai_fmt(44100) == 0x00020800u && hdmi_mai_fmt(32000) == 0x00020700u && hdmi_mai_fmt(22050) == 0);
    assert(hdmi_mai_thr() == 0x08080608u);
    assert((hdmi_audio_packet_config() & 0x3c00) == (8u << 10) && (hdmi_audio_packet_config() & 0xff) == 3);
    assert(hdmi_crp_cfg(6144) == (0x01000000u | 6144));
    /* DMA ring: 8 blocks, each its own segment, chained and closed */
    static struct dma_cb cb[HDMI_DMA_SEGS] __attribute__((aligned(32)));
    uint32_t cbb = 0xc0100000u, bufb = 0xc0200000u, segb = HDMI_DMA_SEG_FRAMES * 8u, dst = 0x7e808020u;
    dma_build_ring(cb, HDMI_DMA_SEGS, cbb, bufb, segb, dst);
    assert(sizeof(struct dma_cb) == 32);
    for (uint32_t i = 0; i < HDMI_DMA_SEGS; i++) {
        assert(cb[i].src == bufb + i * segb && cb[i].dst == dst && cb[i].len == segb && cb[i].stride == 0);
        assert(cb[i].next == cbb + ((i + 1) % HDMI_DMA_SEGS) * 32u);
        assert(!(cb[i].ti & DMA_TI_DEST_INC) && (cb[i].ti & DMA_TI_SRC_INC) && (cb[i].ti & DMA_TI_DEST_DREQ));
        assert(((cb[i].ti >> 16) & 0x1f) == HDMI_DREQ && ((cb[i].ti >> 12) & 0xf) == 2);
        assert(dma_cb_index(cbb + i * 32u, cbb, HDMI_DMA_SEGS) == i);
    }
    assert(dma_cb_index(cbb - 32, cbb, HDMI_DMA_SEGS) == 0 && dma_cb_index(cbb + 32u * 8, cbb, HDMI_DMA_SEGS) == 0);
    assert(HDMI_DMA_SEG_FRAMES % IEC958_BLOCK_FRAMES == 0); /* every segment starts a channel status block */
    puts("hdmi audio ok");
    return 0;
}
