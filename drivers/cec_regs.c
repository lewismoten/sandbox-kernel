/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Bit-timing words for the BCM2835 CEC block. */
#include "cec_regs.h"

#define N(us) ((us) / CEC_TICK_US)

int cec_regs_timing(uint32_t hsm_hz, struct cec_timing *t) {
    uint32_t div = hsm_hz / CEC_CLOCK_HZ;
    if (!hsm_hz || div == 0 || div > CEC_C1_DIV_MASK) return -1;
    t->div = div;
    t->c2 = (N(1500u) << 24) | (N(1300u) << 17) | (N(800u) << 11) | (N(600u) << 5) | N(400u);
    t->c3 = (N(2750u) << 24) | (N(2400u) << 16) | (N(2050u) << 8) | N(1700u);
    t->c4 = (N(4300u) << 24) | (N(3900u) << 16) | (N(3600u) << 8) | N(3500u);
    t->c5 = (N(4700u) << CEC_C5_4700_SHIFT) | (N(4500u) << CEC_C5_4500_SHIFT);
    return 0;
}
