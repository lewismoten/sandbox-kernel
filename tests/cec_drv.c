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
    int tx_started, c5_resets;
    uint8_t tx[16]; unsigned tx_len;
    uint8_t sent[32][16]; unsigned sent_len[32]; unsigned sent_n;
};
static uint32_t crd(void *c, uint32_t o) { return ((struct chip *)c)->r[o / 4]; }
static void cwr(void *c, uint32_t o, uint32_t v) {
    struct chip *h = c;
    if (o == CEC_R_CPU_CLEAR) { h->r[CEC_R_CPU_STATUS / 4] &= ~v; return; }
    if (o == CEC_R_CNTRL_1) {
        uint32_t old = h->r[o / 4], ro = CEC_C1_TX_STATUS_GOOD | CEC_C1_REC_WRD_MASK;
        if ((v & CEC_C1_START_XMIT) && !(old & CEC_C1_START_XMIT)) {
            h->tx_started = 1; h->tx_len = ((v & CEC_C1_LEN_MASK) >> CEC_C1_LEN_SHIFT) + 1u;
            for (unsigned i = 0; i < h->tx_len; i++) h->tx[i] = (uint8_t)(h->r[CEC_R_TX_DATA / 4 + i / 4] >> (8 * (i % 4)));
            memcpy(h->sent[h->sent_n], h->tx, h->tx_len); h->sent_len[h->sent_n++] = h->tx_len;
        }
        h->r[o / 4] = (v & ~ro) | (old & ro); return;
    }
    if (o == CEC_R_CNTRL_5) { if (v & CEC_C5_TX_SW_RESET) h->c5_resets++; h->r[o / 4] = v & ~(CEC_C5_TX_SW_RESET | CEC_C5_RX_SW_RESET); return; }
    h->r[o / 4] = v;
}
static void finish_tx_(struct chip *h, int good, int line) {      /* the chip reports the end of a send */
    if (!h->tx_started) fprintf(stderr, "finish_tx at line %d with no send\n", line);
    assert(h->tx_started); h->tx_started = 0;
    h->r[CEC_R_CNTRL_1 / 4] = (h->r[CEC_R_CNTRL_1 / 4] & ~CEC_C1_TX_STATUS_GOOD) | (good ? CEC_C1_TX_STATUS_GOOD : 0);
    h->r[CEC_R_CNTRL_5 / 4] &= ~CEC_C5_RX_CEC_INT; h->r[CEC_R_CPU_STATUS / 4] |= CEC_CPU_CEC;
}
#define finish_tx(h, g) finish_tx_(h, g, __LINE__)
static void inject(struct chip *h, const uint8_t *b, unsigned n) {
    memset(&h->r[CEC_R_RX_DATA / 4], 0, 16);
    for (unsigned i = 0; i < n; i++) h->r[CEC_R_RX_DATA / 4 + i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
    h->r[CEC_R_CNTRL_1 / 4] = (h->r[CEC_R_CNTRL_1 / 4] & ~CEC_C1_REC_WRD_MASK) | ((n - 1u) << CEC_C1_REC_WRD_SHIFT);
    h->r[CEC_R_CNTRL_5 / 4] |= CEC_C5_RX_CEC_INT; h->r[CEC_R_CPU_STATUS / 4] |= CEC_CPU_CEC;
}

static struct { uint16_t code[16]; uint8_t val[16]; unsigned n; } keys;
static void ksink(void *c, uint16_t code, uint8_t v) { (void)c; keys.code[keys.n] = code; keys.val[keys.n++] = v; }

static struct chip chip; static struct cec_drv drv;
static void start(uint16_t phys) {
    memset(&chip, 0, sizeof chip); memset(&drv, 0, sizeof drv); keys.n = 0;
    drv.rd = crd; drv.wr = cwr; drv.key = ksink; drv.ctx = &chip;
    cec_drv_init(&drv, phys, 163680000u, 0);
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
    static const uint8_t rpa[] = {0x4F, 0x84, 0x21, 0x00, 0x04}, osd[] = {0x4F, 0x47, 'G','a','m','e',' ','M','a','k','e','r'};
    static const uint8_t ivo[] = {0x40, 0x04}, asrc[] = {0x4F, 0x82, 0x21, 0x00};
    expect(&chip, 1, rpa, 5);
    finish_tx(&chip, 0); cec_drv_task(&drv, 20); expect(&chip, 2, osd, 12);       /* broadcasts: the flag says nack, no retry */
    finish_tx(&chip, 0); cec_drv_task(&drv, 30); expect(&chip, 3, ivo, 2);
    finish_tx(&chip, 1); cec_drv_task(&drv, 40); expect(&chip, 4, asrc, 4);        /* the TV acked Image View On */
    finish_tx(&chip, 0); cec_drv_task(&drv, 50);
    assert(chip.sent_n == 5 && !chip.tx_started && !drv.tx_busy);                   /* nothing resent */
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
    cec_drv_task(&drv, 1001);                                                       /* transport error after 1 s */
    cec_drv_task(&drv, 1300);                                                       /* after the hold-off the retry goes out */
    assert(chip.c5_resets == 2 && strstr(drv.log.text, "timed out"));
    assert(!(chip.r[CEC_R_CNTRL_1 / 4] & CEC_C1_START_XMIT) || drv.tx_busy);        /* retry of the same poll started */
    assert(chip.sent_n == 2 && drv.sm.state == CEC_SM_POLL);
}

static void claim4(void) {
    start(0x1000); cec_drv_task(&drv, 1); finish_tx(&chip, 0);
    for (unsigned t = 2; t < 20; t++) { cec_drv_task(&drv, t); if (chip.tx_started) finish_tx(&chip, 1); }
    cec_drv_task(&drv, 20); assert(!drv.tx_busy);
    assert(drv.sm.state == CEC_SM_CLAIMED && chip.sent_n == 5);
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
    static const uint8_t rpa[] = {0x4F, 0x84, 0x10, 0x00, 0x04}, ms[] = {0x40, 0x8E, 0x01}, cv[] = {0x40, 0x9E, 0x05};
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
    struct cec_timing t; assert(cec_regs_timing(0, &t) < 0 && cec_regs_timing(40000u * 4096u, &t) < 0 && cec_regs_timing(40000, &t) == 0 && t.div == 1);

    static struct cec_log l; char out[512];
    cec_log_init(&l);
    for (unsigned i = 0; i < 200; i++) cec_log_line(&l, i * 7u, "RX 0>4 44:01 padding padding padding padding");
    assert(l.len <= CEC_LOG_BYTES && l.dropped > 0 && l.text[l.len - 1] == '\n' && l.text[0] >= '0' && l.text[0] <= '9');
    assert(l.lines == 200 - l.dropped);
    assert(strstr(l.text, "1393 RX"));                      /* the newest line is kept */
    uint32_t n = cec_log_tail(&l, 2, out, sizeof out);
    unsigned nl = 0; for (uint32_t i = 0; i < n; i++) nl += out[i] == '\n';
    assert(nl == 2 && strstr(out, "1393 RX") && strstr(out, "1386 RX") && !strstr(out, "1379 RX"));
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
 inject(&chip,(uint8_t[]){0x0f,0x82,0x10,0},4);cec_drv_task(&drv,8);assert(wake_count==3);
 inject(&chip,(uint8_t[]){0x08,0x44,1},3);cec_drv_task(&drv,9);assert(wake_count==3);
 inject(&chip,(uint8_t[]){0x04,0x44},2);cec_drv_task(&drv,10);assert(wake_count==3);
 inject(&chip,(uint8_t[]){0x0f,0x82},2);cec_drv_task(&drv,11);assert(wake_count==3);
 drv.sm.qn=CEC_SM_QUEUE;assert(cec_sm_standby(&drv.sm)==-1);
}

int main(void) {
    test_power();
    test_timing_and_claim(); puts("PASS timing and claim");
    test_next_candidate_and_none(); puts("PASS candidates and silence");
    test_timeout(); puts("PASS send timeout");
    test_rx_keys_and_replies(); puts("PASS rx, keys, replies");
    test_registers_wiped(); puts("PASS registers wiped and rewritten");
    test_edid_log_timing(); puts("PASS edid, log, timing");
    return 0;
}
