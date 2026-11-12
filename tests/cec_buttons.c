/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Host replay of a TV remote pressing every button: a poll, then every UI
 * code 0x00-0x7F as Pressed+Released at human speed and in a fast burst, and the other frames a TV sends
 * (Standby, Routing Change, Set Stream Path, Request Active Source, Give Device Power Status, Menu Request,
 * Vendor Remote Button Down/Up, vendor commands, Set Menu Language, Abort). Asserts each cec_drv_task pass does
 * a bounded amount of register work, nothing loops or storms, the key events stay balanced and valid, and a
 * pass stays cheap when the log is full. The fake chip is the one in tests/cec_drv.c. */
#include "cec_drv.h"
#include "looptrace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PASS_MAX_ACCESSES 160u   /* register reads+writes in one pass; a normal busy pass is about 40 */
#define CAP 4096u


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
static unsigned accesses, worst;
static uint32_t crd2(void *c, uint32_t o) { accesses++; return crd(c, o); }
static void cwr2(void *c, uint32_t o, uint32_t v) { accesses++; cwr(c, o, v); }

static struct { unsigned n, down, up, rep, bad; int held[768]; } ks;
static void ksink(void *c, uint16_t code, uint8_t v) {
    (void)c; ks.n++;
    if (!code || code >= 768u || v > 2u) { ks.bad++; return; }          /* what input_frame would refuse */
    if (v == 1) { ks.down++; ks.held[code]++; } else if (v == 0) { ks.up++; ks.held[code]--; } else ks.rep++;
}
static unsigned wakes;
static void kwake(void *c) { (void)c; wakes++; }

static struct chip chip; static struct cec_drv drv; static uint32_t T;
static void boot(uint16_t phys) {
    memset(&chip, 0, sizeof chip); memset(&drv, 0, sizeof drv); memset(&ks, 0, sizeof ks); wakes = 0; T = 0; worst = 0;
    drv.rd = crd2; drv.wr = cwr2; drv.key = ksink; drv.wake = kwake; drv.ctx = &chip;
    cec_drv_init(&drv, phys, 163682864u, 0);
}
/* one main-loop pass at time T, then a good ack for any frame the chip started */
static void pass(uint32_t t) {
    T = t; accesses = 0; cec_drv_task(&drv, T);
    if (accesses > worst) worst = accesses;
    assert(accesses <= PASS_MAX_ACCESSES);
    assert(drv.sm.qn <= CEC_SM_QUEUE);
    if (chip.tx_started) { finish_tx(&chip, 1); }
}
static void settle(uint32_t ms) { uint32_t b = T; for (uint32_t t = b + 5; t <= b + ms; t += 5) pass(t); T = b + ms; }
static void claim(void) {
    boot(0x1000); cec_drv_task(&drv, 1); finish_tx(&chip, 0); pass(10);       /* poll 4 unanswered: claimed */
    settle(2000);
    assert(drv.sm.state == CEC_SM_CLAIMED && drv.sm.laddr == 4);
}
/* a frame from the TV (initiator 0), at time t; returns the number of frames we sent in the next 300 ms */
static unsigned rx(uint32_t t, const uint8_t *body, unsigned n) {
    uint8_t f[16]; f[0] = 0x04; memcpy(f + 1, body, n); inject(&chip, f, n + 1);
    unsigned before = chip.sent_n; pass(t); for (uint32_t k = t + 5; k <= t + 300; k += 5) pass(k);
    T = t + 300; return chip.sent_n - before;
}
static unsigned rxb(uint32_t t, const uint8_t *body, unsigned n) {      /* broadcast from the TV */
    uint8_t f[16]; f[0] = 0x0F; memcpy(f + 1, body, n); inject(&chip, f, n + 1);
    unsigned before = chip.sent_n; pass(t); for (uint32_t k = t + 5; k <= t + 300; k += 5) pass(k);
    T = t + 300; return chip.sent_n - before;
}
static void balanced(void) {
    settle(1500);                                               /* the 550 ms release timer ends any press left open */
    for (unsigned i = 0; i < 768; i++) assert(ks.held[i] == 0);
    assert(ks.bad == 0);
}

static void test_every_ui_code_human_and_burst(void) {
    claim();
    for (int burst = 0; burst < 2; burst++) {
        unsigned total = 0, rx0 = drv.rx_frames;
        for (unsigned ui = 0; ui < 0x80; ui++) {
            uint8_t pr[3] = {0x44, (uint8_t)ui}, rl[1] = {0x45};
            unsigned before = chip.sent_n;
            if (!burst) { rx(T + 100, pr, 2); rx(T + 100, rl, 1); }   /* human speed: ~300 ms between frames */
            else {                                                    /* a burst: a frame every 25 ms, no replies awaited */
                uint8_t f[3] = {0x04, 0x44, (uint8_t)ui}; inject(&chip, f, 3); pass(T + 25);
                uint8_t g[2] = {0x04, 0x45}; inject(&chip, g, 2); pass(T + 50); T += 50;
            }
            total += chip.sent_n - before;
        }
        if (!burst) assert(drv.rx_frames - rx0 == 256u);               /* human speed: every frame was read */
        else assert(drv.rx_frames - rx0 > 200u);
        printf("  %s: %u of 256 frames read\n", burst ? "burst" : "human", drv.rx_frames - rx0);
        assert(total == 0);                                           /* a button never makes us transmit */
        balanced();
    }
    assert(ks.down > 0 && ks.up == ks.down);
    printf("PASS every UI code, human and burst (worst pass %u register accesses)\n", worst);
}

static void test_other_frames(void) {
    claim();
    static const struct { const char *name; uint8_t b[8]; unsigned n; int bc; unsigned max_tx; } f[] = {
        { "Standby", {0x36}, 1, 0, 1 }, { "Standby bcast", {0x36}, 1, 1, 0 },
        { "Routing Change", {0x80, 0x10, 0x00, 0x20, 0x00}, 5, 1, 0 },
        { "Routing Information", {0x81, 0x20, 0x00}, 3, 1, 0 },
        { "Set Stream Path other", {0x86, 0x30, 0x00}, 3, 1, 0 }, { "Set Stream Path us", {0x86, 0x10, 0x00}, 3, 1, 3 },
        { "Request Active Source", {0x85}, 1, 1, 1 }, { "Give Device Power Status", {0x8F}, 1, 0, 1 },
        { "Menu Request", {0x8D, 0x00}, 2, 0, 1 }, { "Vendor Remote Down", {0x8A, 0x01, 0x02}, 3, 0, 1 },
        { "Vendor Remote Up", {0x8B}, 1, 0, 1 }, { "Vendor Command", {0x89, 0x01, 0x02, 0x03}, 4, 0, 1 },
        { "Vendor Command With ID", {0xA0, 0x00, 0x09, 0xC0, 0x11, 0x22}, 6, 0, 1 },
        { "Set Menu Language", {0x32, 'e', 'n', 'g'}, 4, 1, 0 }, { "Abort", {0xFF}, 1, 0, 1 },
        { "Feature Abort", {0x00, 0x44, 0x04}, 3, 0, 0 }, { "Deck status req", {0x1A, 0x01}, 2, 0, 1 },
        { "Report Power On", {0x90, 0x00}, 2, 0, 3 }, { "Give Device Vendor ID", {0x8C}, 1, 1, 1 },
    };
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
        for (int again = 0; again < 3; again++) {                  /* three times: nothing builds up */
            unsigned n = f[i].bc ? rxb(T + 100, f[i].b, f[i].n) : rx(T + 100, f[i].b, f[i].n);
            if (n > f[i].max_tx) { fprintf(stderr, "%s sent %u frames (max %u)\n", f[i].name, n, f[i].max_tx); assert(0); }
        }
    }
    settle(3000); assert(drv.sm.qn == 0 && !drv.tx_busy);
    printf("PASS TV frames: each answered at most as listed, queue drained\n");
}

static void test_storm(void) {                                       /* 2000 frames, 2 ms apart, every opcode and UI code: a pass never exceeds its bound */
    claim(); unsigned before = chip.sent_n;
    for (unsigned i = 0; i < 2000; i++) {
        uint8_t f[4] = { (uint8_t)((i & 1) ? 0x04 : 0x0F), (uint8_t)(i * 7u), (uint8_t)(i * 13u), (uint8_t)i };
        if (!(i & 1)) f[0] = 0x04;
        inject(&chip, f, 2 + (i % 3)); pass(T + 2);
        T += 2;
    }
    settle(5000);
    assert(drv.sm.qn == 0 && !drv.tx_busy);
    assert(chip.sent_n - before < 2400u);                            /* at most about one reply per frame */
    balanced();
    printf("PASS storm of 2000 mixed frames, %u frames sent, worst pass %u accesses\n", chip.sent_n - before, worst);
}

static void test_full_log(void) {                                    /* a pass with the 24 KiB log at its limit stays cheap (the line drop is a copy, not a loop of them) */
    claim();
    for (unsigned i = 0; i < 3000; i++) { uint8_t pr[2] = {0x44, (uint8_t)(i % 0x30)}; uint8_t f[3] = {0x04, pr[0], pr[1]}; inject(&chip, f, 3); pass(T + 30); T += 30; }
    assert(drv.log.len <= CEC_LOG_BYTES);
    printf("PASS 3000 key frames into a full log, len %u\n", (unsigned)drv.log.len);
}

static void test_looptrace(void) {
    char b[80]; looptrace0.passes = 3; looptrace_note(4, 1500); looptrace_note(1, 20); looptrace_note(99, 5);
    unsigned n = looptrace_take(b, sizeof b, 1);
    assert(!strcmp(b, "loop 3 passes, max 1500 us in os, os running") && n == strlen(b));
    assert(looptrace0.passes == 0 && looptrace0.max_us[4] == 0);
    n = looptrace_take(b, 12, 0); assert(n == 11 && b[11] == 0);                  /* cut to the buffer, always terminated */
    printf("PASS loop breadcrumb line\n");
}

int main(void) {
    test_looptrace();
    test_every_ui_code_human_and_burst();
    test_other_frames();
    test_storm();
    test_full_log();
    return 0;
}
