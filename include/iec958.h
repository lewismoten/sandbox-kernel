/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Pure code, no registers: pack S16 stereo
 * frames into IEC 60958 (consumer, "IEC 958") subframes as HDMI audio sample
 * packets carry them, and the audio clock regeneration N/CTS values.
 *
 * Sources: IEC 60958-1/-3 (subframe layout, parity, consumer channel status);
 * HDMI 1.4b section 7.2 and tables 7-1 to 7-3 (N and CTS for 32/44.1/48 kHz).
 * ALSA alsa-lib src/pcm/pcm_iec958.c (LGPL-2.1-or-later) was read for the
 * documented subframe layout and B/M/W tag values only; no code copied.
 * Written from the public facts, not from any driver. Every constant marked
 * VERIFY was recalled and must be checked against the spec text before the
 * hardware slice relies on it.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define IEC958_BLOCK_FRAMES 192u
/* Subframe word: bits 0-3 preamble tag, 4-27 sample (24-bit slot, 16-bit PCM
 * sits in bits 12-27, the low 8 bits of the slot are zero), 28 valid (0 = good
 * audio), 29 user data (0), 30 channel status bit, 31 parity (bits 4-31 even). */
enum { IEC958_PRE_B = 0x8, IEC958_PRE_M = 0x2, IEC958_PRE_W = 0x4 }; /* B/Z: block start, M/X: other left, W/Y: right; ALSA IEC958_SUBFRAME_LE */
/* Consumer channel status for 2-channel 16-bit PCM at `rate` (48000, 44100 or
 * 32000). Byte 4 says 16-bit samples (max 20 bits, sample word length 16:
 * 0x02; VERIFY) to match the 16-bit PCM in a 24-bit subframe slot. Fills 24 bytes (192 bits): bit n of the block rides frame n.
 * `right` sets the channel number (1 left, 2 right). Returns 0, or -1 for an
 * unsupported rate. */
int iec958_status(uint32_t rate, int right, uint8_t out[24]);
/* Status bit for frame `n` (0..191) from a 24-byte block. */
static inline unsigned iec958_status_bit(const uint8_t b[24], unsigned n) { return (b[n >> 3] >> (n & 7)) & 1u; }
uint32_t iec958_parity(uint32_t word); /* 0 or 1: the bit that makes bits 4-31 even */
/* One subframe: preamble tag, S16 sample, channel status bit. */
uint32_t iec958_subframe(unsigned pre, int16_t sample, unsigned cbit);
/* Pack `frames` stereo S16 frames (ring format: left in the low half of each
 * word) into 2 subframes each: out[2*i] left, out[2*i+1] right. `first` is the
 * frame number within the 192-frame block of the first frame (0..191); it
 * wraps. Returns the next frame number. `status` is the 24-byte block for
 * `rate` from iec958_status (left, right: two blocks, or one if both equal in
 * the caller's use; pass both). */
unsigned iec958_pack(const uint32_t *pcm, uint32_t frames, unsigned first,
                     const uint8_t statusL[24], const uint8_t statusR[24], uint32_t *out);
/* HDMI audio clock regeneration: 128*fs = f_pix * N / CTS. Returns 0 and fills
 * n and cts, or -1 when the pair (rate, pixel clock in Hz) is not in the table. */
int hdmi_nct(uint32_t rate, uint32_t pixel_hz, uint32_t *n, uint32_t *cts);
