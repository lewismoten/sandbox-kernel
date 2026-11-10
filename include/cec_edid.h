/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). The CEC physical address from a display's EDID. */
#ifndef CEC_EDID_H
#define CEC_EDID_H
#include <stdint.h>
#define CEC_EDID_BLOCK 128u
/* 1 when the 128 bytes sum to zero mod 256 (a valid EDID block). */
int cec_edid_checksum_ok(const uint8_t *blk);
/* Physical address from one CEA-861 extension block (tag 2): the HDMI vendor block (OUI
 * 00-0C-03) holds it right after the OUI. Returns it (never 0xFFFF), or -1 if this block
 * has none or is malformed. */
int cec_edid_phys(const uint8_t *blk);
#endif
