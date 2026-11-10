/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5), from the public CEA-861 / EDID layout (as described in
 * the HDMI 1.4 spec, section 8.7: the source's address is the one in the sink's HDMI vendor block). */
#include "cec_edid.h"

int cec_edid_checksum_ok(const uint8_t *blk) {
    unsigned sum = 0;
    for (unsigned i = 0; i < CEC_EDID_BLOCK; i++) sum += blk[i];
    return (sum & 0xffu) == 0;
}

int cec_edid_phys(const uint8_t *blk) {
    if (blk[0] != 0x02 || !cec_edid_checksum_ok(blk)) return -1;
    unsigned end = blk[2];                      /* first detailed timing; data blocks start at 4 */
    if (end < 4 || end > CEC_EDID_BLOCK - 1u) return -1;
    for (unsigned i = 4; i < end;) {
        unsigned tag = blk[i] >> 5, len = blk[i] & 0x1fu;
        if (i + 1 + len > end) return -1;
        if (tag == 3 && len >= 5 && blk[i + 1] == 0x03 && blk[i + 2] == 0x0c && blk[i + 3] == 0x00) {
            unsigned p = (unsigned)(blk[i + 4] << 8) | blk[i + 5];
            return p == 0xffffu ? -1 : (int)p;
        }
        i += 1 + len;
    }
    return -1;
}
