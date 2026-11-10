/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). HDMI audio on the BCM2835 (Pi Zero W):
 * the pure part (clock choice, N/CTS, audio infoframe, DMA control-block
 * chain, register values) is host-tested; drivers/hdmi_audio.c touches the
 * hardware. Register facts only, from public sources; no driver code copied:
 *  - BCM2835 ARM Peripherals datasheet (public): DMA controller registers,
 *    control block layout, DREQ pacing, bus address aliases.
 *  - Linux drivers/gpu/drm/vc4/vc4_hdmi.c, vc4_hdmi_regs.h and vc4_regs.h
 *    (GPL-2.0): read only for register offsets, bit positions and the order
 *    of steps (facts). Nothing was copied. Circle (GPLv3) was not read.
 *  - HDMI 1.4b and CEA-861 for N/CTS and the Audio InfoFrame (type 0x84,
 *    version 1, length 10).
 * Items marked VERIFY are recalled or inferred and wait for a hardware check.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Peripheral window: ARM 0x20000000 = bus 0x7E000000. */
#define HDMI_HD_BASE   0x20808000u /* "HD" block (bus 0x7E808000): MAI FIFO */
#define HDMI_CORE_BASE 0x20902000u /* HDMI core (bus 0x7E902000): packets, CTS */
#define HD_M_CTL       0x00c
#define HD_MAI_CTL     0x014
#define HD_MAI_THR     0x018
#define HD_MAI_FMT     0x01c
#define HD_MAI_DATA    0x020
#define HD_MAI_SMP     0x02c
#define HC_MAI_CHANNEL_MAP 0x090
#define HC_MAI_CONFIG  0x094
#define HC_AUDIO_PACKET_CONFIG 0x09c
#define HC_RAM_PACKET_CONFIG 0x0a0
#define HC_RAM_PACKET_STATUS 0x0a4
#define HC_CRP_CFG     0x0a8
#define HC_CTS_0       0x0ac
#define HC_CTS_1       0x0b0
#define HC_RAM_PACKET_START 0x400
#define HC_PACKET_STRIDE 0x24
#define PKT_ID_AUDIO_INFO 4u /* infoframe type 0x84 - 0x80 */
/* MAI_CTL bits */
#define MAI_CTL_RESET 0x1u
#define MAI_CTL_ERRORF 0x2u
#define MAI_CTL_ERRORE 0x4u
#define MAI_CTL_ENABLE 0x8u
#define MAI_CTL_CHNUM_SHIFT 4
#define MAI_CTL_PAREN 0x100u
#define MAI_CTL_FLUSH 0x200u
#define MAI_CTL_EMPTY 0x400u
#define MAI_CTL_FULL 0x800u
#define MAI_CTL_WHOLSMP 0x1000u
#define MAI_CTL_CHALIGN 0x2000u
#define MAI_CTL_BUSY 0x4000u
#define MAI_CTL_DLATE 0x8000u
/* AUDIO_PACKET_CONFIG, CRP_CFG, MAI_CONFIG */
#define APC_ZERO_ON_SAMPLE_FLAT 0x20000000u
#define APC_ZERO_ON_INACTIVE 0x01000000u
#define APC_B_FRAME_ID_SHIFT 10
#define CRP_EXTERNAL_CTS_EN 0x01000000u
#define MAI_CONFIG_FORMAT_REVERSE 0x08000000u
#define MAI_CONFIG_BIT_REVERSE 0x04000000u

struct hdmi_audio_clocks {
    uint32_t n, cts;        /* HDMI audio clock regeneration */
    uint32_t smp_n, smp_m;  /* MAI_SMP divider fields: hsm/rate ~ smp_n/(smp_m+1) written as n<<8 | m */
    uint32_t mai_hz;        /* resulting sample rate in Hz (rounded) for logging */
};
/* Best fraction n/m of num/den with n <= max_n and 1 <= m <= max_m (continued
 * fractions, 32-bit division only). Returns 0, or -1 for a zero argument. */
int hdmi_rational(uint32_t num, uint32_t den, uint32_t max_n, uint32_t max_m, uint32_t *n, uint32_t *m);
/* 148.5 MHz 1080p60 (CEA mode 16), 48 kHz is the kernel's mode. hsm_hz is the
 * HSM clock that feeds the MAI sample divider. Returns 0 or -1. */
int hdmi_audio_clocks(uint32_t rate, uint32_t pixel_hz, uint32_t hsm_hz, struct hdmi_audio_clocks *out);
/* HSM clock rate from its Clock Manager CTL and DIV words. src: 1 oscillator
 * (19.2 MHz), 6 PLLD_PER (500 MHz: hardware log 2026-11-09 read CTL 0x2d6, DIV 0x30e0 = 163.68 MHz); other sources
 * unknown (0). Integer divider only plus the 12-bit fraction. */
uint32_t hdmi_hsm_hz(uint32_t ctl, uint32_t div);
/* Audio InfoFrame for 2-channel LPCM: 4 header bytes (type, version, length,
 * checksum) and 10 payload bytes, 14 in all. PB1 = channel count - 1 with
 * coding type 0 ("refer to stream header"), the rest 0 (front left/right). */
#define AUDIO_INFOFRAME_BYTES 14u
void hdmi_audio_infoframe(uint8_t out[AUDIO_INFOFRAME_BYTES]);
/* Pack an infoframe into packet RAM words: 7 bytes make two words (bytes 0-2
 * little-endian, then bytes 3-6), the way the slot is filled. `slot_words`
 * (9: stride 0x24) receives the zero-padded result. */
void hdmi_infoframe_words(const uint8_t *bytes, uint32_t len, uint32_t slot_words[9]);
/* Register values for 2-channel PCM. */
uint32_t hdmi_mai_ctl_run(void);        /* CHNUM 2, WHOLSMP, CHALIGN, ENABLE */
uint32_t hdmi_mai_fmt(uint32_t rate);   /* PCM format 2, rate code (48000 = 9); 0 for an unlisted rate */
uint32_t hdmi_mai_thr(void);            /* panic 8/8, dreq 6/8 */
uint32_t hdmi_audio_packet_config(void);
uint32_t hdmi_crp_cfg(uint32_t n);
uint32_t hdmi_mai_channel_map(void);    /* two channels: 0, 1<<3 (VERIFY) */

/* DMA control blocks (BCM2835 peripherals datasheet section 4). 32 bytes, 32-aligned. */
struct dma_cb { uint32_t ti, src, dst, len, stride, next, pad[2]; };
#define DMA_TI_INTEN (1u << 0)
#define DMA_TI_WAIT_RESP (1u << 3)
#define DMA_TI_DEST_INC (1u << 4)
#define DMA_TI_DEST_DREQ (1u << 6)
#define DMA_TI_SRC_INC (1u << 8)
#define DMA_TI_BURST(n) ((uint32_t)(n) << 12)
#define DMA_TI_PERMAP(n) ((uint32_t)(n) << 16)
#define HDMI_DREQ 17u /* DMA peripheral map for the HDMI audio FIFO (VERIFY) */
#define DMA_BUS_RAM 0xC0000000u /* uncached alias of ARM physical RAM */
#define HDMI_DMA_SEG_FRAMES 384u /* two IEC 958 blocks (192 frames each): 8 ms, starts on a block boundary */
#define HDMI_DMA_SEGS 8u
/* Build a ring of `nseg` control blocks. cb_bus = bus address of cb[0]; buf_bus
 * = bus address of the buffer, seg_bytes each; dst_bus = MAI_DATA bus address.
 * Each block moves one segment (32-bit writes, no destination increment,
 * paced by the HDMI DREQ, burst 2) and chains to the next, the last back to
 * the first. */
void dma_build_ring(struct dma_cb *cb, uint32_t nseg, uint32_t cb_bus, uint32_t buf_bus,
                    uint32_t seg_bytes, uint32_t dst_bus);
/* Index of the block whose bus address is `conblk` (0 if outside the ring). */
uint32_t dma_cb_index(uint32_t conblk, uint32_t cb_bus, uint32_t nseg);

/* Start / stop and service, for the kernel. */
int hdmi_audio_init(void);      /* 0 = the HDMI backend replaces the null backend; else -1 and logs why */
int hdmi_audio_active(void);
void hdmi_audio_task(void);     /* main-loop hook: refill DMA segments from the ring */
int hdmi_audio_tone(uint32_t hz, uint32_t ms); /* quiet test tone through the mixer; 0 or -1 */
void hdmi_audio_report(char *out, size_t cap);  /* status text for the USB AUDIO command */
