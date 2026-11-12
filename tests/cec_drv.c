/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Host tests for the CEC transport against a fake register file (the chip's behavior is what
 * the Linux vc4 driver implies: this checks our sequences, not the silicon). Also the EDID
 * address finder, the bounded log and the timing words. */
#include "cec_drv.h"
#include "cec_edid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct chip {
    uint32_t r[0x400 / 4];
    int tx_started, c5_resets, rx_resets, clear_pulses;
    uint8_t tx[16]; unsigned tx_len;
    int sticky_rx; uint8_t sent[128][16]; unsigned sent_len[128]; unsigned sent_n;
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
            memcpy(h->sent[h->sent_n % 128], h->tx, h->tx_len); h->sent_len[h->sent_n++ % 128] = h->tx_len;
        }
        if ((v & CEC_C1_START_XMIT) && !(old & CEC_C1_START_XMIT)) old &= ~CEC_C1_TX_EOM;   /* a new send clears the end flag */
        if ((v & CEC_C1_CLEAR_RECEIVE_OFF) && !(old & CEC_C1_CLEAR_RECEIVE_OFF)) {          /* the receiver is re-armed */
            h->clear_pulses++; if (!h->sticky_rx) { old &= ~CEC_C1_RX_EOM; h->r[CEC_R_CNTRL_5 / 4] &= ~CEC_C5_RX_CEC_INT; }
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

static struct { uint16_t code[16]; uint8_t val[16]; unsigned n; } keys;
static void ksink(void *c, uint16_t code, uint8_t v) { (void)c; keys.code[keys.n] = code; keys.val[keys.n++] = v; }

static struct chip chip; static struct cec_drv drv;
static void start(uint16_t phys) {
    memset(&chip, 0, sizeof chip); memset(&drv, 0, sizeof drv); keys.n = 0;
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
    /* a bit that never clears turns the flag path off instead of spinning */
    claim4(); chip.sticky_rx = 1; inject_quiet(&chip, up, 3);
    for (uint32_t t = 1000; t < 1400; t += 40) cec_drv_task(&drv, t);
    assert(drv.rx_flag_off && drv.rx_frames <= 2 && strstr(drv.log.text, "flag path off"));
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
    for (unsigned i = 0; i < 3000; i++) cec_log_line(&l, 1u + i, "send timed out; c1 00144FFC c5 004CBCB4 st 00000002 hpd 0");
    assert(l.len <= CEC_LOG_BYTES && l.dropped > 0 && !strncmp(l.text, "0 session boot x\n", 17) && strstr(l.text, "\n3000 send"));
    assert(l.text[l.len - 1] == '\n' && l.text[l.len] == 0);
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
    test_edid_log_timing(); puts("PASS edid, log, timing");
    test_late_tv(); puts("PASS TV that appears 60 s late");
    return 0;
}
