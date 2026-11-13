/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Host tests for the CEC transport against a fake register file (the chip's behavior is what
 * the Linux vc4 driver implies: this checks our sequences, not the silicon). Also the EDID
 * address finder, the bounded log and the timing words. */
#include "cec_drv.h"
#include "cec_edid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CAP 256u   /* captured frames; the late-TV case needs 57. More is counted in `lost`, never written. */
struct chip {
    uint32_t r[0x400 / 4];
    int tx_started, c5_resets, rx_resets, clear_pulses;
    uint8_t tx[16]; unsigned tx_len;
    int sticky_rx, sticky_int; uint8_t sent[CAP][16]; unsigned sent_len[CAP]; unsigned sent_n, lost;
};
static uint32_t crd(void *c, uint32_t o) { return ((struct chip *)c)->r[o / 4]; }
static void cwr(void *c, uint32_t o, uint32_t v) {
    struct chip *h = c;
    if (o == CEC_R_CPU_CLEAR) { h->r[CEC_R_CPU_STATUS / 4] &= ~v; return; }
    if (o == CEC_R_CNTRL_1) {
        uint32_t old = h->r[o / 4], ro = CEC_C1_TX_STATUS_GOOD | CEC_C1_REC_WRD_MASK | CEC_C1_TX_EOM | CEC_C1_RX_EOM;
        if ((v & CEC_C1_START_XMIT) && !(old & CEC_C1_START_XMIT)) {
            h->tx_started = 1; h->tx_len = ((v & CEC_C1_LEN_MASK) >> CEC_C1_LEN_SHIFT) + 1u;
            for (unsigned i = 0; i < h->tx_len; i++) h->tx[i] = (uint8_t)(h->r[CEC_R_TX_DATA / 4 + i / 4] >> (8 * (i % 4)));
            if (h->sent_n >= CAP) h->lost++;
            else { memcpy(h->sent[h->sent_n], h->tx, h->tx_len); h->sent_len[h->sent_n++] = h->tx_len; }
        }
        if ((v & CEC_C1_START_XMIT) && !(old & CEC_C1_START_XMIT)) old &= ~CEC_C1_TX_EOM;   /* a new send clears the end flag */
        if ((v & CEC_C1_CLEAR_RECEIVE_OFF) && !(old & CEC_C1_CLEAR_RECEIVE_OFF)) {          /* the receiver is re-armed */
            h->clear_pulses++; if (!h->sticky_rx) { old &= ~CEC_C1_RX_EOM; if (!h->sticky_int) h->r[CEC_R_CNTRL_5 / 4] &= ~CEC_C5_RX_CEC_INT; }
        }
        h->r[o / 4] = (v & ~ro) | (old & ro); return;
    }
    if (o == CEC_R_CNTRL_5) { if (v & CEC_C5_TX_SW_RESET) h->c5_resets++; if (v & CEC_C5_RX_SW_RESET) h->rx_resets++; h->r[o / 4] = v & ~(CEC_C5_TX_SW_RESET | CEC_C5_RX_SW_RESET); return; }
    h->r[o / 4] = v;
}
static void finish_tx_(struct chip *h, int good, int line) {      /* the chip reports the end of a send */
    if (!h->tx_started) fprintf(stderr, "finish_tx at line %d with no send\n", line);
    assert(h->tx_started); h->tx_started = 0;
    h->r[CEC_R_CNTRL_1 / 4] = (h->r[CEC_R_CNTRL_1 / 4] & ~CEC_C1_TX_STATUS_GOOD) | (good ? CEC_C1_TX_STATUS_GOOD : 0) | CEC_C1_TX_EOM;
    h->r[CEC_R_CNTRL_5 / 4] &= ~CEC_C5_RX_CEC_INT; h->r[CEC_R_CPU_STATUS / 4] |= CEC_CPU_CEC;
}
#define finish_tx(h, g) finish_tx_(h, g, __LINE__)
static void inject(struct chip *h, const uint8_t *b, unsigned n) {
    memset(&h->r[CEC_R_RX_DATA / 4], 0, 16);
    for (unsigned i = 0; i < n; i++) h->r[CEC_R_RX_DATA / 4 + i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
    h->r[CEC_R_CNTRL_1 / 4] = (h->r[CEC_R_CNTRL_1 / 4] & ~CEC_C1_REC_WRD_MASK) | ((n - 1u) << CEC_C1_REC_WRD_SHIFT);
    h->r[CEC_R_CNTRL_5 / 4] |= CEC_C5_RX_CEC_INT; h->r[CEC_R_CPU_STATUS / 4] |= CEC_CPU_CEC; h->r[CEC_R_CNTRL_1 / 4] |= CEC_C1_RX_EOM;
}
/* the same frame with the CPU status bit never rising (what the Insignia log suggests happened) */
static void inject_quiet(struct chip *h, const uint8_t *b, unsigned n) { inject(h, b, n); h->r[CEC_R_CPU_STATUS / 4] &= ~CEC_CPU_CEC; }

static struct { uint16_t code[16]; uint8_t val[16]; unsigned n, lost; } keys;
static void ksink(void *c, uint16_t code, uint8_t v) { (void)c; if (keys.n >= 16) { keys.lost++; return; } keys.code[keys.n] = code; keys.val[keys.n++] = v; }

static struct chip chip; static struct cec_drv drv;
static void start(uint16_t phys) {
    memset(&chip, 0, sizeof chip); memset(&drv, 0, sizeof drv); keys.n = 0; keys.lost = 0;
    drv.rd = crd; drv.wr = cwr; drv.key = ksink; drv.ctx = &chip;
    cec_drv_init(&drv, phys, 163682864u, 0);
}
static void expect(struct chip *h, unsigned i, const uint8_t *e, unsigned n) {
    if (!(h->sent_n > i && h->sent_len[i] == n && !memcmp(h->sent[i], e, n))) { fprintf(stderr, "frame %u: have %u frames", i, h->sent_n); if (h->sent_n > i) { fprintf(stderr, " len %u:", h->sent_len[i]); for (unsigned k = 0; k < h->sent_len[i]; k++) fprintf(stderr, " %02X", h->sent[i][k]); } fprintf(stderr, "\n"); }
    assert(h->sent_n > i && h->sent_len[i] == n && memcmp(h->sent[i], e, n) == 0);
}

static void test_timing_and_claim(void) {
    start(0x2100);
    assert((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_DIV_MASK) == 4092u);              /* 163.68 MHz / 40 kHz */
    assert(((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_ADDR_MASK) >> CEC_C1_ADDR_SHIFT) == 15u);
    assert(chip.r[CEC_R_CNTRL_2 / 4] == ((60u << 24) | (52u << 17) | (32u << 11) | (24u << 5) | 16u));
    assert(chip.r[CEC_R_CNTRL_3 / 4] == ((110u << 24) | (96u << 16) | (82u << 8) | 68u));
    assert(chip.r[CEC_R_CNTRL_4 / 4] == ((172u << 24) | (156u << 16) | (144u << 8) | 140u));
    assert((chip.r[CEC_R_CNTRL_5 / 4] & 0xffffu) == ((188u << 8) | 180u) && chip.c5_resets == 1);
    cec_drv_task(&drv, 1);                                                          /* poll for address 4 starts */
    static const uint8_t poll4[] = {0x44};
    expect(&chip, 0, poll4, 1);
    assert((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_START_XMIT) && ((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_LEN_MASK) >> 16) == 0);
    finish_tx(&chip, 0);                                                           /* nobody answers: free */
    cec_drv_task(&drv, 10);
    assert(drv.sm.state == CEC_SM_CLAIMED && drv.sm.laddr == 4);
    assert(((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_ADDR_MASK) >> CEC_C1_ADDR_SHIFT) == 4u);
    static const uint8_t rpa[] = {0x4F, 0x84, 0x21, 0x00, 0x04}, osd[] = {0x40, 0x47, 'G','a','m','e',' ','M','a','k','e','r'};
    static const uint8_t ivo[] = {0x40, 0x04}, asrc[] = {0x4F, 0x82, 0x21, 0x00};
    expect(&chip, 1, rpa, 5);
    finish_tx(&chip, 0); cec_drv_task(&drv, 20); expect(&chip, 2, osd, 12);       /* broadcast Report: the flag says nack, no retry */
    finish_tx(&chip, 1); cec_drv_task(&drv, 30); expect(&chip, 3, ivo, 2);
    finish_tx(&chip, 1); cec_drv_task(&drv, 40); expect(&chip, 4, asrc, 4);        /* the TV acked Image View On */
    finish_tx(&chip, 0); cec_drv_task(&drv, 50); expect(&chip, 5, (uint8_t[]){0x40, 0x8E, 0x00}, 3); /* Menu Status activated */
    finish_tx(&chip, 1); cec_drv_task(&drv, 60);
    assert(chip.sent_n == 6 && !chip.tx_started && !drv.tx_busy);                   /* nothing resent */
    assert(strstr(drv.log.text, "claimed logical address 4") && strstr(drv.log.text, "announcing"));
}

static void test_next_candidate_and_none(void) {
    start(0x1000);
    cec_drv_task(&drv, 1); finish_tx(&chip, 1); cec_drv_task(&drv, 2);              /* 4 is taken */
    static const uint8_t poll8[] = {0x88}, poll11[] = {0xBB};
    expect(&chip, 1, poll8, 1); finish_tx(&chip, 1); cec_drv_task(&drv, 3);
    expect(&chip, 2, poll11, 1); finish_tx(&chip, 1); cec_drv_task(&drv, 4);
    assert(drv.sm.state == CEC_SM_NO_ADDRESS && !drv.tx_busy && strstr(drv.log.text, "no logical address"));
    cec_drv_task(&drv, 5000); assert(chip.sent_n == 3);                             /* stays silent */
    start(CEC_PHYS_UNKNOWN); cec_drv_task(&drv, 1); assert(chip.sent_n == 0 && drv.sm.state == CEC_SM_NO_ADDRESS);
    memset(&chip, 0, sizeof chip); memset(&drv, 0, sizeof drv);
    drv.rd = crd; drv.wr = cwr; drv.key = ksink; drv.ctx = &chip;
    cec_drv_init(&drv, 0x1000, 0, 0); cec_drv_task(&drv, 1);                         /* no HSM clock: off */
    assert(chip.sent_n == 0 && strstr(drv.log.text, "CEC off"));
}

static void test_timeout(void) {
    start(0x1000);
    cec_drv_task(&drv, 1); assert(chip.tx_started && drv.tx_busy);
    cec_drv_task(&drv, 500); assert(drv.tx_busy);
    cec_drv_task(&drv, 2001);                    /* no ACK: claim, cancel the chip before announcements */
    assert(chip.c5_resets == 2 && strstr(drv.log.text, "poll 4 timeout"));
    assert(drv.sm.state == CEC_SM_CLAIMED && drv.sm.laddr == 4 && !drv.tx_busy);
    assert(!(chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_START_XMIT));
    assert(chip.sent_n == 1 && !strstr(drv.log.text, "poll 4 nack"));
    cec_drv_task(&drv, 2300);
    expect(&chip, 1, (uint8_t[]){0x4f, 0x84, 0x10, 0, 4}, 5);
    /* A latched completion wins over the deadline, even with unrelated status bit 1. */
    for (int good = 0; good <= 1; good++) {
        start(0x1000); cec_drv_task(&drv, 1);
        chip.r[CEC_R_CPU_STATUS / 4] |= 2u;
        finish_tx(&chip, good); cec_drv_task(&drv, 2001);
        assert(!strstr(drv.log.text, "timeout"));
        assert(strstr(drv.log.text, good ? "poll 4 ack" : "poll 4 nack"));
        assert(drv.sm.state == (good ? CEC_SM_POLL : CEC_SM_CLAIMED));
        assert(chip.c5_resets == 1);
    }
    /* c5 bit 23 high since before the send (a latched cause): read once at most, then neither a completion nor a timeout reads it as a frame */
    for (int mode = 0; mode < 2; mode++) {
        start(0x1000); chip.sticky_int = 1; chip.r[CEC_R_CNTRL_5 / 4] |= CEC_C5_RX_CEC_INT;
        cec_drv_task(&drv, 1);
        cec_drv_task(&drv, 40);
        if (mode) { chip.r[CEC_R_CPU_STATUS / 4] |= CEC_CPU_CEC; finish_tx(&chip, 1); cec_drv_task(&drv, 100); assert(strstr(drv.log.text, "poll 4 ack")); }
        else { cec_drv_task(&drv, 2100); assert(strstr(drv.log.text, "timeout")); }
        assert(drv.rx_frames <= 1);                                  /* the one rise is read once, never again */
        if (!mode) { unsigned f = drv.rx_frames; cec_drv_task(&drv, 2300); cec_drv_task(&drv, 2400); assert(drv.rx_frames == f); }
    }
}

static void claim4(void) {
    start(0x1000); cec_drv_task(&drv, 1); finish_tx(&chip, 0);
    for (unsigned t = 2; t < 20; t++) { cec_drv_task(&drv, t); if (chip.tx_started) finish_tx(&chip, 1); }
    cec_drv_task(&drv, 20); assert(!drv.tx_busy);
    assert(drv.sm.state == CEC_SM_CLAIMED && chip.sent_n == 6);
}

static void test_rx_keys_and_replies(void) {
    claim4(); keys.n = 0;
    static const uint8_t up[] = {0x04, 0x44, 0x01}, unk[] = {0x04, 0x44, 0x66}, rel[] = {0x04, 0x45};
    inject(&chip, up, 3); cec_drv_task(&drv, 100);
    assert(keys.n == 1 && keys.code[0] == 103 && keys.val[0] == 1);
    assert(((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_CLEAR_RECEIVE_OFF)) == 0);             /* the clear pulse ended */
    assert(!(chip.r[CEC_R_CPU_STATUS / 4] & CEC_CPU_CEC));
    inject(&chip, up, 3); cec_drv_task(&drv, 200); assert(keys.n == 2 && keys.val[1] == 2);   /* repeat */
    inject(&chip, rel, 2); cec_drv_task(&drv, 250); assert(keys.n == 3 && keys.val[2] == 0);
    inject(&chip, unk, 3); cec_drv_task(&drv, 300); assert(keys.n == 3 && strstr(drv.log.text, "UI 66 UNMAPPED"));
    static const uint8_t sel[] = {0x04, 0x44, 0x00}, back[] = {0x04, 0x44, 0x0D};
    inject(&chip, sel, 3); cec_drv_task(&drv, 1000); assert(keys.code[keys.n - 1] == 28 && keys.val[keys.n - 1] == 1);
    cec_drv_task(&drv, 1600); assert(keys.val[keys.n - 1] == 0);                       /* no more Pressed: released after 550 ms */
    inject(&chip, back, 3); cec_drv_task(&drv, 2000); assert(keys.code[keys.n - 1] == 1);
    /* replies */
    unsigned before = chip.sent_n;
    static const uint8_t gosd[] = {0x04, 0x46}, gpa[] = {0x04, 0x83}, mreq[] = {0x04, 0x8D, 0x00}, gv[] = {0x04, 0x9F};
    static const uint8_t osdr[] = {0x40, 0x47, 'G','a','m','e',' ','M','a','k','e','r'};
    static const uint8_t rpa[] = {0x4F, 0x84, 0x10, 0x00, 0x04}, ms[] = {0x40, 0x8E, 0x00}, cv[] = {0x40, 0x9E, 0x05};
    inject(&chip, gosd, 2); cec_drv_task(&drv, 3000); expect(&chip, before, osdr, 12); finish_tx(&chip, 1); cec_drv_task(&drv, 3005);
    inject(&chip, gpa, 2); cec_drv_task(&drv, 3010); expect(&chip, before + 1, rpa, 5); finish_tx(&chip, 0); cec_drv_task(&drv, 3020);
    inject(&chip, mreq, 3); cec_drv_task(&drv, 3030); expect(&chip, before + 2, ms, 3); finish_tx(&chip, 1); cec_drv_task(&drv, 3040);
    inject(&chip, gv, 2); cec_drv_task(&drv, 3050); expect(&chip, before + 3, cv, 3); finish_tx(&chip, 1); cec_drv_task(&drv, 3060);
    assert(chip.sent_n == before + 4);
    assert(strstr(drv.log.text, "RX 0>4 46"));
}

static void test_registers_wiped(void) {
    claim4(); keys.n = 0;
    chip.r[CEC_R_CNTRL_1 / 4] &= ~(CEC_C1_DIV_MASK | CEC_C1_ADDR_MASK);              /* a video mode change resets the block */
    chip.r[CEC_R_CNTRL_2 / 4] = 0;
    cec_drv_task(&drv, 1000);
    assert(drv.reprogrammed == 1 && strstr(drv.log.text, "registers programmed again"));
    assert((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_DIV_MASK) == 4092u && ((chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_ADDR_MASK) >> 12) == 4u);
    assert(chip.r[CEC_R_CNTRL_2 / 4] != 0);
    cec_drv_task(&drv, 1200); assert(drv.reprogrammed == 1);                         /* stable: no more writes */
    static const uint8_t up[] = {0x04, 0x44, 0x01};
    inject(&chip, up, 3); cec_drv_task(&drv, 1300); assert(keys.n == 1);             /* still receiving */
}

static void test_flags_without_status_bit(void) {
    /* the unmask Linux does on enable */
    start(0x1000); assert(chip.r[CEC_R_CPU_MASK_CLEAR / 4] & CEC_CPU_CEC);
    /* a send that ends (EOM) without the CPU status bit ever rising is still seen */
    cec_drv_task(&drv, 1); assert(chip.tx_started);
    chip.r[CEC_R_CNTRL_1 / 4] |= CEC_C1_TX_EOM;                              /* a free address: EOM, not good */
    cec_drv_task(&drv, 20); assert(drv.sm.state == CEC_SM_CLAIMED && strstr(drv.log.text, "poll 4 nack") && strstr(drv.log.text, "c1 flag"));
    /* a frame with the status bit clear: c5 bit 23 and RX_EOM only */
    claim4(); keys.n = 0;
    static const uint8_t up[] = {0x04, 0x44, 0x01};
    inject_quiet(&chip, up, 3); cec_drv_task(&drv, 1000);
    assert(keys.n == 1 && keys.code[0] == 103 && keys.val[0] == 1 && drv.rx_via_flags == 1);
    assert(strstr(drv.log.text, "rx raw 3B 04:44:01 via flags") && strstr(drv.log.text, "receive flag, status bit clear;"));
    cec_drv_task(&drv, 1100); assert(drv.rx_frames == 1);                    /* cleared: not read twice */
    /* a bit that never clears does not make it spin */
    claim4(); chip.sticky_rx = 1; inject_quiet(&chip, up, 3);
    for (uint32_t t = 1000; t < 1400; t += 40) cec_drv_task(&drv, t);
    assert(drv.rx_frames <= 2);
    /* only c5 bit 23 stays high (a latched cause), RX_EOM clears: one frame at most, no key repeats */
    claim4(); keys.n = 0; chip.sticky_int = 1; inject_quiet(&chip, up, 3); chip.r[CEC_R_CNTRL_1 / 4] &= ~CEC_C1_RX_EOM;
    for (uint32_t t = 1000; t < 3000; t += 30) cec_drv_task(&drv, t);
    unsigned presses = 0; for (unsigned i = 0; i < keys.n; i++) presses += keys.val[i] == 1 || keys.val[i] == 2;
    assert(drv.rx_frames <= 1 && presses <= 1);
    /* a stale bit 23 beside a finished send is not a received frame, and a send that times out does not replay an old frame */
    claim4(); chip.sticky_int = 0; chip.r[CEC_R_CNTRL_5 / 4] |= CEC_C5_RX_CEC_INT; chip.r[CEC_R_CPU_STATUS / 4] |= CEC_CPU_CEC;
    { unsigned f = drv.rx_frames; for (uint32_t t = 100; t < 4000; t += 40) cec_drv_task(&drv, t); assert(drv.rx_frames <= f + 1); }
    /* the same frame read again within 20 ms is dropped; a repeat 100 ms later is a new press */
    claim4(); keys.n = 0; inject(&chip, up, 3); cec_drv_task(&drv, 5000); inject(&chip, up, 3); cec_drv_task(&drv, 5010);
    assert(keys.n == 1 && drv.rx_dups == 1);
    inject(&chip, up, 3); cec_drv_task(&drv, 5110); assert(keys.n == 2);
    /* a second init starts clean */
    drv.rx_flag_off = 1; drv.rx_flag_bad = 5; drv.tx_eom_stale = 1; drv.flags_sig = 7; drv.flags_ms = 9; drv.rx_flag_ms = 9; drv.rx_via_flags = 3; drv.rx_pending = 1;
    cec_drv_init(&drv, 0x1000, 163682864u, 0);
    assert(!drv.rx_flag_off && !drv.rx_flag_bad && !drv.tx_eom_stale && !drv.flags_sig && !drv.flags_ms && !drv.rx_flag_ms && !drv.rx_via_flags && !drv.rx_pending);
    /* the normal path logs every received frame raw, key frames too */
    claim4(); inject(&chip, up, 3); cec_drv_task(&drv, 100);
    assert(strstr(drv.log.text, "rx raw 3B 04:44:01 via status"));
}

static void test_reset_keeps_receiver(void) {
    claim4(); keys.n = 0;
    int rx0 = chip.rx_resets;
    inject(&chip, (uint8_t[]){0x04, 0x46}, 2); cec_drv_task(&drv, 100);        /* Give OSD Name: a reply goes out */
    assert(drv.tx_busy);
    cec_drv_task(&drv, 2200);                                                  /* it never finishes: the transmitter is reset */
    assert(!drv.tx_busy && strstr(drv.log.text, "transmitter reset") && chip.rx_resets == rx0);   /* the receiver was left alone */
    static const uint8_t up[] = {0x04, 0x44, 0x01};
    inject(&chip, up, 3); cec_drv_task(&drv, 2300);
    assert(keys.n == 1 && keys.code[0] == 103);                                /* still receiving after the reset */
    /* a frame that arrives in the pass where the deadline passes is read before the send is cancelled */
    claim4(); keys.n = 0;
    inject(&chip, (uint8_t[]){0x04, 0x46}, 2); cec_drv_task(&drv, 100); assert(drv.tx_busy);
    inject(&chip, up, 3); cec_drv_task(&drv, 2200);
    assert(keys.n == 1 && strstr(drv.log.text, "transmitter reset"));
    /* a receiver flagged when a send times out is read and re-armed (the TV spoke while we waited) */
    claim4(); keys.n = 0;
    inject(&chip, (uint8_t[]){0x04, 0x46}, 2); cec_drv_task(&drv, 100); assert(drv.tx_busy);
    inject_quiet(&chip, up, 3); cec_drv_task(&drv, 2200);
    assert(keys.n == 1 && strstr(drv.log.text, "receiver flagged at timeout"));
}

static void test_log_keeps_head(void) {
    static struct cec_log l; cec_log_init(&l);
    cec_log_line(&l, 0, "session boot x");
    for (unsigned i = 0; i < 3000; i++) cec_log_line(&l, 1u + i * 40u, "send timed out; c1 00144FFC c5 004CBCB4 st 00000002 hpd 0");   /* 25 a second: under the rate limit */
    assert(l.len <= CEC_LOG_BYTES && l.dropped > 0 && !strncmp(l.text, "0 session boot x\n", 17) && strstr(l.text, "\n119961 send") && !l.shed);
    assert(l.text[l.len - 1] == '\n' && l.text[l.len] == 0);
}

/* A 1,000-frame remote storm (one every 2 ms): the log takes at most CEC_LOG_RATE lines a window, counts the rest,
   the head survives, and the end is marked "dropped K lines" even when nothing else is logged afterwards. */
static void test_log_storm(void) {
    static struct cec_log l; cec_log_init(&l);
    cec_log_line(&l, 0, "session boot x"); cec_log_line(&l, 5, "POWER HDMI blank, CEC standby");
    for (unsigned i = 0; i < 1000; i++) { cec_log_line(&l, 100u + i * 2u, "rx 04:44:41 key"); assert(l.len <= CEC_LOG_BYTES); }
    unsigned kept = 0; for (uint32_t i = 0; i < l.len; i++) kept += l.text[i] == '\n';
    assert(kept <= 2u * CEC_LOG_RATE + 2u && l.shed > 900u);
    assert(!strncmp(l.text, "0 session boot x\n", 17) && strstr(l.text, "POWER HDMI blank"));
    uint32_t shed = l.shed; cec_log_flush(&l, 2100u + 5u);     /* the storm is over; the main loop's flush writes the marker */
    char want[48]; snprintf(want, sizeof want, "dropped %u lines (rate limit)\n", (unsigned)shed);
    assert(l.shed == 0 && l.len > strlen(want) && !strcmp(l.text + l.len - strlen(want), want));
    uint32_t len = l.len; cec_log_flush(&l, 9000u); assert(l.len == len);   /* nothing more to say */
}

static void test_edid_log_timing(void) {
    uint8_t b[128] = {0};
    b[0] = 0x02; b[1] = 3; b[2] = 12;                      /* data blocks 4..11, then timings */
    b[4] = (1 << 5) | 3; b[5] = 1; b[6] = 2; b[7] = 3;     /* an audio-ish block first */
    b[8] = (3 << 5) | 6; b[9] = 0x03; b[10] = 0x0C; b[11] = 0x00; /* needs 7 bytes: 8..14 */
    b[2] = 16; b[12] = 0x21; b[13] = 0x00; b[14] = 0;
    unsigned sum = 0; for (unsigned i = 0; i < 127; i++) sum += b[i]; b[127] = (uint8_t)(0x100u - (sum & 0xffu)); if (sum % 256 == 0) b[127] = 0;
    assert(cec_edid_checksum_ok(b) && cec_edid_phys(b) == 0x2100);
    b[0] = 0x00; assert(cec_edid_phys(b) < 0); b[0] = 2;
    b[127] ^= 1; assert(cec_edid_phys(b) < 0); b[127] ^= 1;
    b[9] = 0x04; sum = 0; for (unsigned i = 0; i < 127; i++) sum += b[i]; b[127] = (uint8_t)(0x100u - (sum & 0xffu));
    assert(cec_edid_phys(b) < 0);                           /* not the HDMI OUI */
    b[9] = 0x03; b[12] = 0xff; b[13] = 0xff; sum = 0; for (unsigned i = 0; i < 127; i++) sum += b[i]; b[127] = (uint8_t)(0x100u - (sum & 0xffu));
    assert(cec_edid_phys(b) < 0);                           /* 0xFFFF means none */
    struct cec_timing t;
    static const struct { uint32_t hz, div; } clocks[] = {
        {40000u, 1u}, {120000000u, 3000u}, {163682864u, 4092u},
        {4095u * 40000u + 39999u, 4095u}
    };
    for (unsigned i = 0; i < sizeof clocks / sizeof clocks[0]; i++) {
        assert(cec_regs_timing(clocks[i].hz, &t) == 0 && t.div == clocks[i].div);
    }
    assert(cec_regs_timing(39999u, &t) < 0);
    assert(cec_regs_timing(0, &t) < 0 && cec_regs_timing(40000u * 4096u, &t) < 0 && cec_regs_timing(40000, &t) == 0 && t.div == 1);

    static struct cec_log l; char out[512];
    cec_log_init(&l);
    for (unsigned i = 0; i < 1000; i++) cec_log_line(&l, i * 7u, "RX 0>4 44:01 padding padding padding padding");
    assert(l.len <= CEC_LOG_BYTES && l.dropped > 0 && l.text[l.len - 1] == '\n' && l.text[0] >= '0' && l.text[0] <= '9');
    assert(l.lines == 1000 - l.dropped);
    assert(strstr(l.text, "6993 RX"));                      /* the newest line is kept */
    uint32_t n = cec_log_tail(&l, 2, out, sizeof out);
    unsigned nl = 0; for (uint32_t i = 0; i < n; i++) nl += out[i] == '\n';
    assert(nl == 2 && strstr(out, "6993 RX") && strstr(out, "6986 RX") && !strstr(out, "6979 RX"));
    cec_log_line(&l, 1, "bell\x07 x"); assert(strstr(l.text, "bell? x"));
}

static unsigned wake_count;
static void wake_sink(void *ctx) {(void)ctx;wake_count++;}
static void test_power(void) {
 start(0x2100);drv.wake=wake_sink;wake_count=0;
 /* Claim without pending discovery traffic, to isolate power frames. */
 drv.sm.state=CEC_SM_CLAIMED;drv.sm.laddr=4;drv.sm.qn=0;
 drv.last_state=CEC_SM_CLAIMED;drv.announced=1;
 assert(!cec_sm_standby(&drv.sm));cec_drv_task(&drv,1);
 expect(&chip,0,(uint8_t[]){0x40,0x36},2);assert(!drv.sm.active);
 finish_tx(&chip,1);cec_drv_task(&drv,2);
 cec_sm_announce(&drv.sm);cec_drv_task(&drv,3);
 expect(&chip,1,(uint8_t[]){0x40,0x04},2);
 finish_tx(&chip,1);cec_drv_task(&drv,4);
 expect(&chip,2,(uint8_t[]){0x4f,0x82,0x21,0},4);
 finish_tx(&chip,1);cec_drv_task(&drv,5);
 inject(&chip,(uint8_t[]){0x04,0x44,0x66},3);cec_drv_task(&drv,6);assert(wake_count==1);
 inject(&chip,(uint8_t[]){0x04,0x04},2);cec_drv_task(&drv,7);assert(wake_count==2);
 inject(&chip,(uint8_t[]){0x0f,0x82,0x10,0},4);cec_drv_task(&drv,8);assert(wake_count==2); /* foreign Active Source: no wake */
 inject(&chip,(uint8_t[]){0x08,0x44,1},3);cec_drv_task(&drv,9);assert(wake_count==2);
 inject(&chip,(uint8_t[]){0x04,0x44},2);cec_drv_task(&drv,10);assert(wake_count==2);
 inject(&chip,(uint8_t[]){0x0f,0x82},2);cec_drv_task(&drv,11);assert(wake_count==2);
 drv.sm.qn=CEC_SM_QUEUE;assert(cec_sm_standby(&drv.sm)==-1);
}

/* A TV that is off at boot, starts 60 s later, raises HPD, and at first NACKs directed frames (still starting). */
static unsigned tv_phys_calls;
static int late_phys(void *c) { (void)c; tv_phys_calls++; return chip.r[CEC_R_HOTPLUG / 4] ? 0x2100 : -1; }
static int sent_has(const uint8_t *e, unsigned n) {
    for (unsigned i = 0; i < chip.sent_n; i++) if (chip.sent_len[i] == n && !memcmp(chip.sent[i], e, n)) return 1;
    return 0;
}
static int rise_phys(void *c) { (void)c; return 0x2100; }
/* No logical address (all polls answered): a display that rises or changes address must still restart the claim. */
static void test_no_address_refresh(void) {
    start(CEC_PHYS_UNKNOWN); drv.phys = rise_phys;
    cec_drv_task(&drv, 1); assert(drv.sm.state == CEC_SM_NO_ADDRESS && chip.sent_n == 0);
    chip.r[CEC_R_HOTPLUG / 4] = 1; cec_drv_task(&drv, 20); cec_drv_task(&drv, 40);
    assert(drv.sm.phys == 0x2100 && drv.hpd_rises == 1 && drv.restarts == 1 && drv.sm.state == CEC_SM_POLL);
    start(0x2100); drv.phys = rise_phys;                                        /* all three polls answered */
    cec_drv_task(&drv, 1); finish_tx(&chip, 1); cec_drv_task(&drv, 2); finish_tx(&chip, 1); cec_drv_task(&drv, 3); finish_tx(&chip, 1); cec_drv_task(&drv, 4);
    assert(drv.sm.state == CEC_SM_NO_ADDRESS && !drv.tx_busy);
    unsigned sent = chip.sent_n; chip.r[CEC_R_HOTPLUG / 4] = 0; cec_drv_task(&drv, 100); chip.r[CEC_R_HOTPLUG / 4] = 1; cec_drv_task(&drv, 200);
    assert(drv.hpd_rises == 1 && sent == chip.sent_n);                          /* same address: still silent, but the rise was seen */
}
static void test_late_tv(void) {
    start(0x1000); drv.phys = late_phys; drv.phys_assumed = 1; tv_phys_calls = 0;
    unsigned before_up = 0, claimed_ms = 0;
    for (uint32_t t = 1; t <= 90000u; t += 20u) {
        if (t >= 60000u && !chip.r[CEC_R_HOTPLUG / 4]) { before_up = chip.sent_n; chip.r[CEC_R_HOTPLUG / 4] = 1; }
        cec_drv_task(&drv, t);
        if (chip.tx_started && t >= 60500u) {                      /* the bus works from 60.5 s */
            int to_tv = (chip.tx[0] & 15u) == 0;
            finish_tx(&chip, to_tv ? (t >= 63000u) : (chip.tx_len > 1));   /* directed to the TV: NACK until 63 s; polls free */
        }
        if (!claimed_ms && drv.sm.state == CEC_SM_CLAIMED && !drv.sm.tentative && drv.bus_ok) claimed_ms = t;
    }
    assert(before_up >= 4);                                          /* it kept trying while the TV was dark, not for 11 s only */
    assert(drv.restarts >= 3 && drv.backoff_ms <= 30000u);
    assert(claimed_ms >= 60500u && claimed_ms < 66000u);            /* a verified claim within seconds of the bus working */
    assert(drv.sm.phys == 0x2100 && !drv.phys_assumed && drv.hpd_rises == 1);
    assert(sent_has((uint8_t[]){0x4F, 0x84, 0x21, 0x00, 0x04}, 5)); /* Report Physical Address with the address read late */
    assert(sent_has((uint8_t[]){0x4F, 0x82, 0x21, 0x00}, 4));       /* Active Source */
    assert(strstr(drv.log.text, "hotplug rose") && strstr(drv.log.text, "done; c1") && strstr(drv.log.text, " hpd "));
    /* the TV reports it is on later (it booted after our announcement): announce again */
    unsigned n = chip.sent_n;
    inject(&chip, (uint8_t[]){0x0F, 0x90, 0x00}, 3); cec_drv_task(&drv, 90020);
    for (uint32_t t = 90040; t < 90400; t += 20) { cec_drv_task(&drv, t); if (chip.tx_started) finish_tx(&chip, 1); }
    assert(chip.sent_n >= n + 2 && chip.sent_len[n] == 4 && chip.sent[n][1] == 0x82);
}

/* The capture is bounded: more frames than it holds are counted as lost, nothing is written past the arrays. */
static void test_capture_bounds(void) {
    start(0x1000);
    for (unsigned i = 0; i < CAP + 40u; i++) {
        chip.r[CEC_R_TX_DATA / 4] = 0x4Fu;
        cwr(&chip, CEC_R_CNTRL_1, 0); cwr(&chip, CEC_R_CNTRL_1, CEC_C1_START_XMIT);
    }
    assert(chip.sent_n == CAP && chip.lost == 40u);
    for (unsigned i = 0; i < 40u; i++) ksink(0, 103, 1);
    assert(keys.n == 16u && keys.lost == 24u);
    chip.lost = 0; keys.lost = 0;
}

/* The display's address read fails after an HPD rise (the TV is still starting): it is read again, never given up. */
static unsigned edid_fail_left, edid_calls; static uint32_t cur_ms, call_ms[8];
static int flaky_phys(void *c) { (void)c; if (edid_calls < 8) call_ms[edid_calls] = cur_ms; edid_calls++; return edid_fail_left ? (edid_fail_left--, -1) : 0x2100; }
static void step(uint32_t t) { cur_ms = t; cec_drv_task(&drv, t); if (chip.tx_started) finish_tx(&chip, chip.tx_len > 1); }
static void test_edid_retry(void) {
    start(0x1000); drv.phys = flaky_phys; drv.phys_assumed = 0; edid_fail_left = 2; edid_calls = 0;
    chip.r[CEC_R_HOTPLUG / 4] = 0; step(10);
    chip.r[CEC_R_HOTPLUG / 4] = CEC_HOTPLUG_CONNECTED;
    for (uint32_t t = 20; t < 20000u; t += 20) step(t);
    /* two failed reads, each retried (at least 3 s apart), then the third succeeds and stops the reads */
    assert(edid_calls == 3 && drv.sm.phys == 0x2100 && !drv.phys_want && !drv.phys_failed && drv.hpd_rises == 1);
    assert(call_ms[1] - call_ms[0] >= 3000u && call_ms[2] - call_ms[1] >= 3000u && call_ms[2] < 12000u);
    /* a fall of the display's HPD cancels a pending retry */
    start(0x1000); drv.phys = flaky_phys; edid_fail_left = 99; edid_calls = 0;
    step(10); chip.r[CEC_R_HOTPLUG / 4] = CEC_HOTPLUG_CONNECTED; step(20);
    chip.r[CEC_R_HOTPLUG / 4] = 0; step(40);
    for (uint32_t t = 60; t < 9000; t += 20) step(t);
    assert(edid_calls == 1 && !drv.phys_want);
}


/* 2026-11-12 session 3 (Insignia): a poll arrives with c5 bit 23 only and the bit stays high; later frames must still be read:
 * the TV polls us, asks for our physical address, then sends User Control Pressed and Released (keys). */
static void test_tv_session_after_sticky_c5(void) {
    claim4(); keys.n = 0; chip.sticky_int = 1;
    static const uint8_t poll[] = {0x04}, gpa[] = {0x04, 0x83}, up[] = {0x04, 0x44, 0x01}, rel[] = {0x04, 0x45};
    inject_quiet(&chip, poll, 1); chip.r[CEC_R_CNTRL_1 / 4] &= ~CEC_C1_RX_EOM;       /* bit 23 only, as logged at 972 ms */
    cec_drv_task(&drv, 2000);
    assert(drv.rx_frames == 1 && strstr(drv.log.text, "rx raw 1B 04 via flags") && strstr(drv.log.text, "RX 0>4"));
    assert(chip.r[CEC_R_CNTRL_5 / 4] & CEC_C5_RX_CEC_INT);                           /* still high after the clear */
    for (uint32_t t = 2040; t < 2600; t += 40) cec_drv_task(&drv, t);               /* quiet: no phantom frames */
    assert(drv.rx_frames == 1);
    unsigned sent = chip.sent_n;
    inject_quiet(&chip, gpa, 2); cec_drv_task(&drv, 3000);                           /* Give Physical Address (broadcast from 0) */
    assert(drv.rx_frames == 2 && strstr(drv.log.text, "RX 0>4 83 (Give Physical Address)"));
    cec_drv_task(&drv, 3040); assert(chip.sent_n == sent + 1 && chip.sent[sent][1] == 0x84);   /* we answer Report Physical Address */
    finish_tx(&chip, 1); cec_drv_task(&drv, 3100);
    inject_quiet(&chip, up, 3); cec_drv_task(&drv, 4000);                            /* the key, with c5 already high */
    assert(keys.n == 1 && keys.code[0] == 103 && keys.val[0] == 1 && drv.rx_frames == 3);
    inject_quiet(&chip, rel, 2); cec_drv_task(&drv, 4100);
    assert(drv.rx_frames == 4 && keys.n == 2 && keys.val[1] == 0);
    inject_quiet(&chip, up, 3); cec_drv_task(&drv, 4400);                            /* the same key again later is a new press */
    assert(keys.n == 3 && keys.val[2] == 1);
    assert(strstr(drv.log.text, "TX 4>15 84:") && strstr(drv.log.text, "(Report Physical Address) broadcast"));
}

int main(void) {
    test_power();
    test_timing_and_claim(); puts("PASS timing and claim");
    test_next_candidate_and_none(); puts("PASS candidates and silence");
    test_timeout(); puts("PASS send timeout");
    test_rx_keys_and_replies(); puts("PASS rx, keys, replies");
    test_registers_wiped(); puts("PASS registers wiped and rewritten");
    test_flags_without_status_bit(); puts("PASS flags without the status bit");
    test_reset_keeps_receiver(); puts("PASS reset keeps the receiver");
    test_log_keeps_head(); puts("PASS log keeps its head");
    test_log_storm(); puts("PASS a 1,000-frame storm is rate limited and its end marked");
    test_edid_log_timing(); puts("PASS edid, log, timing");
    test_no_address_refresh(); puts("PASS no-address driver still refreshes the display address");
    test_tv_session_after_sticky_c5(); puts("PASS TV session with c5 bit 23 stuck high");
    test_late_tv(); assert(!chip.lost); puts("PASS TV that appears 60 s late");
    test_capture_bounds(); puts("PASS capture is bounded");
    test_edid_retry(); puts("PASS a failed EDID read is read again");
    return 0;
}
