/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Pure planning code for HDMI audio; see
 * include/hdmi_audio.h for sources and the VERIFY convention. */
#include "hdmi_audio.h"
#include "iec958.h"
#include <string.h>

int hdmi_rational(uint32_t num, uint32_t den, uint32_t max_n, uint32_t max_m, uint32_t *n, uint32_t *m) {
    if (!num || !den || !max_n || !max_m) return -1;
    uint32_t h0 = 0, h1 = 1, k0 = 1, k1 = 0; /* convergents h/k, k is the denominator */
    uint32_t a = num, b = den;
    for (;;) {
        uint32_t q = a / b, r = a % b;
        uint64_t h2 = (uint64_t)q * h1 + h0, k2 = (uint64_t)q * k1 + k0;
        if (h2 > max_n || k2 > max_m) {
            /* the semiconvergent: the largest t <= q with t*h1+h0 and t*k1+k0 in bounds */
            uint32_t t = q;
            if (h1) { uint32_t tn = (max_n - h0) / h1; if (tn < t) t = tn; }
            if (k1) { uint32_t tm = (max_m - k0) / k1; if (tm < t) t = tm; }
            if (t && t * 2 >= q) { h1 = t * h1 + h0; k1 = t * k1 + k0; } /* at least half of q: closer than the last convergent */
            break;
        }
        h0 = h1; k0 = k1; h1 = (uint32_t)h2; k1 = (uint32_t)k2;
        if (!r) break;
        a = b; b = r;
    }
    if (!k1 || !h1) return -1;
    *n = h1; *m = k1;
    return 0;
}

int hdmi_audio_clocks(uint32_t rate, uint32_t pixel_hz, uint32_t hsm_hz, struct hdmi_audio_clocks *o) {
    if (hdmi_nct(rate, pixel_hz, &o->n, &o->cts)) return -1;
    uint32_t n, m;
    /* hsm / rate = n / m: the divider that makes one sample strobe (Linux
       programs the same ratio; VERIFY on the board by the DMA drain rate). */
    if (hdmi_rational(hsm_hz, rate, 0xffffffu, 256, &n, &m)) return -1;
    o->smp_n = n; o->smp_m = m - 1;
    o->mai_hz = hsm_hz / n * m + (hsm_hz % n) * m / n; /* 32-bit only: (hsm % n) * m < 2^32 */
    return 0;
}

uint32_t hdmi_hsm_hz(uint32_t ctl, uint32_t div) {
    uint32_t src = ctl & 0xf, divi = (div >> 12) & 0xfff, divf = div & 0xfff, hz;
    if (src == 1) hz = 19200000u; else if (src == 6) hz = 500000000u; else return 0; /* PLLD_PER: the 2 GHz PLLD through its /4 channel; the board's HSM divider 3.0547 gave 163.68 MHz */
    if (!divi) return 0;
    uint32_t den = divi * 4096u + divf, q = hz / den, r = hz % den, sh = 0; /* hz * 4096 / den */
    while ((den >> sh) > 0xfffffu) sh++;
    return q * 4096u + (r >> sh) * 4096u / (den >> sh);
}

void hdmi_audio_infoframe(uint8_t o[AUDIO_INFOFRAME_BYTES]) {
    memset(o, 0, AUDIO_INFOFRAME_BYTES);
    o[0] = 0x84; o[1] = 1; o[2] = 10; /* CEA-861: audio, version 1, 10 data bytes */
    o[4] = 1;                          /* PB1: CC = 1 (2 channels), CT = 0 (refer to stream header) */
    uint32_t s = 0; for (unsigned i = 0; i < AUDIO_INFOFRAME_BYTES; i++) s += o[i];
    o[3] = (uint8_t)(0x100u - (s & 0xffu)); /* checksum: all bytes sum to 0 mod 256 */
}

void hdmi_infoframe_words(const uint8_t *b, uint32_t len, uint32_t w[9]) {
    uint8_t t[36] = {0};
    if (len > sizeof t) len = sizeof t;
    memcpy(t, b, len);
    memset(w, 0, 9 * sizeof *w);
    for (unsigned i = 0, o = 0; i + 3 <= 36 && o + 2 <= 9; i += 7, o += 2) {
        w[o] = t[i] | ((uint32_t)t[i + 1] << 8) | ((uint32_t)t[i + 2] << 16);
        if (i + 7 <= 36 || i + 3 < 36) {
            uint32_t v = 0; for (unsigned k = 0; k < 4 && i + 3 + k < 36; k++) v |= (uint32_t)t[i + 3 + k] << (8 * k);
            w[o + 1] = v;
        }
    }
}

uint32_t hdmi_mai_ctl_run(void) { return (2u << MAI_CTL_CHNUM_SHIFT) | MAI_CTL_WHOLSMP | MAI_CTL_CHALIGN | MAI_CTL_ENABLE; }
uint32_t hdmi_mai_fmt(uint32_t rate) {
    static const struct { uint32_t hz, code; } c[] = {{32000,7},{44100,8},{48000,9},{88200,11},{96000,12}};
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) if (c[i].hz == rate) return (2u << 16) | (c[i].code << 8);
    return 0;
}
uint32_t hdmi_mai_thr(void) { return (8u << 24) | (8u << 16) | (6u << 8) | 8u; }
uint32_t hdmi_audio_packet_config(void) { return APC_ZERO_ON_SAMPLE_FLAT | APC_ZERO_ON_INACTIVE | (8u << APC_B_FRAME_ID_SHIFT) | 0x3u; }
uint32_t hdmi_crp_cfg(uint32_t n) { return CRP_EXTERNAL_CTS_EN | (n & 0xfffffu); }
uint32_t hdmi_mai_channel_map(void) { return 1u << 3; }

void dma_build_ring(struct dma_cb *cb, uint32_t nseg, uint32_t cb_bus, uint32_t buf_bus, uint32_t seg_bytes, uint32_t dst_bus) {
    for (uint32_t i = 0; i < nseg; i++) {
        cb[i].ti = DMA_TI_WAIT_RESP | DMA_TI_DEST_DREQ | DMA_TI_SRC_INC | DMA_TI_BURST(2) | DMA_TI_PERMAP(HDMI_DREQ);
        cb[i].src = buf_bus + i * seg_bytes;
        cb[i].dst = dst_bus;
        cb[i].len = seg_bytes;
        cb[i].stride = 0;
        cb[i].next = cb_bus + ((i + 1) % nseg) * (uint32_t)sizeof(struct dma_cb);
        cb[i].pad[0] = cb[i].pad[1] = 0;
    }
}
uint32_t dma_cb_index(uint32_t conblk, uint32_t cb_bus, uint32_t nseg) {
    if (conblk < cb_bus) return 0;
    uint32_t i = (conblk - cb_bus) / (uint32_t)sizeof(struct dma_cb);
    return i < nseg ? i : 0;
}

/* Pixel clock from the visible mode when firmware reports no rate. Standard
 * CEA 60 Hz timings only (1080p 148.5, 720p 74.25, 576p/480p 27, 640x480
 * 25.2 MHz); a scaled-down framebuffer (e.g. 960x540) is assumed to be shown
 * on a 1080p output. Returns 0 when the size is not recognized. */
uint32_t hdmi_mode_pixel_hz(uint32_t w, uint32_t h) {
    if (w >= 1900 && h >= 1000) return 148500000u;
    if (w >= 1200 && h >= 700) return 74250000u;
    if (w >= 900 && w < 1200 && h >= 500 && h <= 600) return 148500000u;
    if (w == 640 && h == 480) return 25200000u;
    if (w >= 700 && w <= 720 && h >= 470 && h <= 580) return 27000000u;
    return 0;
}
int hdmi_pixel_plausible(uint32_t hz) { return hz >= 25000000u && hz <= 600000000u; }
