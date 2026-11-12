/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). The CEC registers inside the BCM2835 HDMI core.
 * The offsets, field positions and bit timing were read (facts only, no code copied) from the
 * Linux kernel's public vc4_hdmi_regs.h, vc4_regs.h and vc4_hdmi.c (rpi-6.6.y, GPL-2.0;
 * see NOTICE.md). Offsets are from the HDMI core base (HDMI_CORE_BASE, hdmi_audio.h). */
#ifndef CEC_REGS_H
#define CEC_REGS_H
#include <stdint.h>

#define CEC_R_CNTRL_1 0x0e8u    /* control and status: length, address, divider, TX/RX flags */
#define CEC_R_CNTRL_2 0x0ecu    /* bit-timing thresholds, 400..1500 us */
#define CEC_R_CNTRL_3 0x0f0u    /* bit-timing thresholds, 1700..2750 us */
#define CEC_R_CNTRL_4 0x0f4u    /* bit-timing thresholds, 3500..4300 us */
#define CEC_R_CNTRL_5 0x0f8u    /* start-bit thresholds, resets, RX interrupt flag */
#define CEC_R_TX_DATA 0x0fcu    /* four words: frame byte i is bits 8*(i%4) of word i/4 */
#define CEC_R_RX_DATA 0x10cu    /* same packing */
#define CEC_R_HOTPLUG 0x00cu    /* HDMI core HOTPLUG: bit 0 = display connected (unverified on silicon; AUDIO.TXT read 0 with a working EDID) */
#define CEC_HOTPLUG_CONNECTED 1u
#define CEC_R_CPU_STATUS 0x340u /* bit 6: a CEC event (TX done or RX frame) */
#define CEC_R_CPU_CLEAR 0x348u  /* write the bit to clear it */
#define CEC_R_CPU_MASK_SET 0x350u
#define CEC_CPU_CEC (1u << 6)

#define CEC_C1_TX_STATUS_GOOD (1u << 30) /* the frame was acknowledged (it makes one retry itself) */
#define CEC_C1_REC_WRD_SHIFT 24u         /* received length minus 1, 4 bits */
#define CEC_C1_REC_WRD_MASK (0xfu << 24)
#define CEC_C1_CLEAR_RECEIVE_OFF (1u << 21) /* set then clear after reading RX data */
#define CEC_C1_START_XMIT (1u << 20)        /* clear, then set to send; clear again when done */
#define CEC_C1_LEN_SHIFT 16u                /* frame length minus 1, 4 bits */
#define CEC_C1_LEN_MASK (0xfu << 16)
#define CEC_C1_ADDR_SHIFT 12u               /* our logical address; 15 = unregistered */
#define CEC_C1_ADDR_MASK (0xfu << 12)
#define CEC_C1_DIV_MASK 0xfffu              /* HSM clock divider for the 40 kHz bit clock */

#define CEC_C5_TX_SW_RESET (1u << 27)
#define CEC_C5_RX_SW_RESET (1u << 26)
#define CEC_C5_RX_CEC_INT (1u << 23)        /* the event was a received frame (else a finished send) */
#define CEC_C5_4700_SHIFT 8u
#define CEC_C5_4500_SHIFT 0u
#define CEC_C5_KEEP_MASK 0x007f0000u        /* the preload field is left as it is */

#define CEC_CLOCK_HZ 40000u
#define CEC_TICK_US 25u                     /* 1000000 / 40000 */

struct cec_timing { uint32_t div, c2, c3, c4, c5; };
/* Divider and threshold words for an HSM clock of hsm_hz; c5 holds only the 4700/4500 fields.
 * Returns 0, or -1 when hsm_hz is 0 or the divider would not fit in 12 bits. */
int cec_regs_timing(uint32_t hsm_hz, struct cec_timing *t);
#endif
