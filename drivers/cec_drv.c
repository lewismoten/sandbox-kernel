/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). CEC transport over the register layout in cec_regs.h.
 * The send and receive sequences follow what Linux's vc4_hdmi.c does (GPL-2.0, facts only; NOTICE.md). */
#include "cec_drv.h"

static const char hexd[] = "0123456789ABCDEF";

static void say(struct cec_drv *d, uint32_t now, const char *t) { cec_log_line(&d->log, now, t); }

static unsigned put_hex(char *o, unsigned n, const uint8_t *b, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
        if (i) o[n++] = ':';
        o[n++] = hexd[b[i] >> 4]; o[n++] = hexd[b[i] & 15u];
    }
    return n;
}
static unsigned put_str(char *o, unsigned n, const char *s) { while (*s) o[n++] = *s++; return n; }
static unsigned put_dec(char *o, unsigned n, uint32_t v) {
    char t[11]; unsigned k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k) o[n++] = t[--k];
    return n;
}
/* "RX 0:4 44:01" style: initiator:dest then the bytes after the header */
static void say_frame(struct cec_drv *d, uint32_t now, const char *tag, const struct cec_msg *m) {
    char t[CEC_LOG_LINE]; unsigned n = put_str(t, 0, tag);
    t[n++] = ' '; n = put_dec(t, n, cec_initiator(m)); t[n++] = '>'; n = put_dec(t, n, cec_dest(m));
    if (m->len > 1) { t[n++] = ' '; n = put_hex(t, n, m->b + 1, (unsigned)m->len - 1u); }
    t[n] = 0; say(d, now, t);
}

static void emit(struct cec_drv *d, const struct cec_key_event *ev, unsigned n) {
    for (unsigned i = 0; i < n; i++) { d->key_events++; if (d->key) d->key(d->ctx, ev[i].code, ev[i].value); }
}

void cec_drv_init(struct cec_drv *d, uint16_t phys, uint32_t hsm_hz, uint32_t now_ms) {
    struct cec_timing t; char l[CEC_LOG_LINE]; unsigned n;
    d->tx_busy = 0; d->announced = 0; d->last_ui = 0; d->ui_seen = 0; d->tx_id = 0;
    d->rx_frames = d->tx_frames = d->tx_good = d->tx_bad = d->key_events = d->spurious = 0;
    cec_log_init(&d->log); cec_keys_init(&d->keys);
    n = put_str(l, 0, "init phys "); n = put_hex(l, n, (uint8_t[]){(uint8_t)(phys >> 8), (uint8_t)phys}, 2);
    n = put_str(l, n, " hsm "); n = put_dec(l, n, hsm_hz); l[n] = 0; say(d, now_ms, l);
    if (cec_regs_timing(hsm_hz, &t)) {
        say(d, now_ms, "no clock divider: CEC off");
        cec_sm_init(&d->sm, CEC_PHYS_UNKNOWN, now_ms); d->last_state = d->sm.state; return;
    }
    n = put_str(l, 0, "divider "); n = put_dec(l, n, t.div); l[n] = 0; say(d, now_ms, l);
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);                 /* unregistered, divider; no TX in progress */
    c1 &= ~(CEC_C1_DIV_MASK | CEC_C1_ADDR_MASK | CEC_C1_START_XMIT);
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 | CEC_C1_ADDR_MASK | t.div);
    uint32_t c5 = d->rd(d->ctx, CEC_R_CNTRL_5) & CEC_C5_KEEP_MASK; /* thresholds, then pulse both resets */
    c5 |= t.c5;
    d->wr(d->ctx, CEC_R_CNTRL_5, c5 | CEC_C5_TX_SW_RESET | CEC_C5_RX_SW_RESET);
    d->wr(d->ctx, CEC_R_CNTRL_5, c5);
    d->wr(d->ctx, CEC_R_CNTRL_2, t.c2); d->wr(d->ctx, CEC_R_CNTRL_3, t.c3); d->wr(d->ctx, CEC_R_CNTRL_4, t.c4);
    d->wr(d->ctx, CEC_R_CPU_CLEAR, CEC_CPU_CEC);                  /* drop any stale event */
    cec_sm_init(&d->sm, phys, now_ms);
    d->last_state = d->sm.state;
    if (phys == CEC_PHYS_UNKNOWN) say(d, now_ms, "no physical address: staying silent");
}

static void send_frame(struct cec_drv *d, const struct cec_msg *m) {
    for (unsigned i = 0; i < m->len; i += 4) {
        uint32_t w = 0;
        for (unsigned k = 0; k < 4 && i + k < m->len; k++) w |= (uint32_t)m->b[i + k] << (8u * k);
        d->wr(d->ctx, CEC_R_TX_DATA + (i >> 2) * 4u, w);
    }
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1) & ~CEC_C1_START_XMIT;
    d->wr(d->ctx, CEC_R_CNTRL_1, c1);                              /* start bit must be cleared in between */
    c1 = (c1 & ~CEC_C1_LEN_MASK) | ((uint32_t)(m->len - 1u) << CEC_C1_LEN_SHIFT) | CEC_C1_START_XMIT;
    d->wr(d->ctx, CEC_R_CNTRL_1, c1);
}

static void claimed_check(struct cec_drv *d, uint32_t now) {
    char l[CEC_LOG_LINE]; unsigned n;
    if (d->sm.state == d->last_state) return;
    d->last_state = d->sm.state;
    if (d->sm.state == CEC_SM_CLAIMED) {
        uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1) & ~CEC_C1_ADDR_MASK;
        d->wr(d->ctx, CEC_R_CNTRL_1, c1 | ((uint32_t)d->sm.laddr << CEC_C1_ADDR_SHIFT));
        n = put_str(l, 0, "claimed logical address "); n = put_dec(l, n, d->sm.laddr); l[n] = 0; say(d, now, l);
        if (!d->announced) { d->announced = 1; cec_sm_announce(&d->sm); say(d, now, "announcing: Image View On, Active Source"); }
    } else if (d->sm.state == CEC_SM_NO_ADDRESS) {
        say(d, now, "no logical address claimed (all polls answered or unclear); silent");
    }
}

static void tx_done(struct cec_drv *d, uint32_t now) {
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);
    int good = (c1 & CEC_C1_TX_STATUS_GOOD) != 0;
    int poll = d->tx.len == 1, bcast = cec_dest(&d->tx) == CEC_ADDR_BROADCAST && !poll;
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 & ~CEC_C1_START_XMIT);
    d->tx_busy = 0; d->tx_frames++;
    if (good) d->tx_good++; else d->tx_bad++;
    char l[CEC_LOG_LINE]; unsigned n = put_str(l, 0, poll ? "poll " : "TX ");
    n = put_dec(l, n, cec_initiator(&d->tx)); l[n++] = '>'; n = put_dec(l, n, cec_dest(&d->tx));
    if (d->tx.len > 1) { l[n++] = ' '; n = put_hex(l, n, d->tx.b + 1, (unsigned)d->tx.len - 1u); }
    n = put_str(l, n, good ? " ack" : " nack"); l[n] = 0; say(d, now, l);
    /* A broadcast is acknowledged by silence, so the hardware's flag says little: never retry one. */
    cec_sm_tx_done(&d->sm, d->tx_id, (good || bcast) ? CEC_SM_TX_ACK : CEC_SM_TX_NACK);
    claimed_check(d, now);
}

static void rx_frame(struct cec_drv *d, uint32_t now) {
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);
    unsigned len = 1u + ((c1 & CEC_C1_REC_WRD_MASK) >> CEC_C1_REC_WRD_SHIFT);
    uint8_t raw[16]; struct cec_msg m;
    for (unsigned i = 0; i < len; i += 4) {
        uint32_t w = d->rd(d->ctx, CEC_R_RX_DATA + (i >> 2) * 4u);
        for (unsigned k = 0; k < 4; k++) raw[i + k] = (uint8_t)(w >> (8u * k));
    }
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 | CEC_C1_CLEAR_RECEIVE_OFF);   /* ready for the next frame */
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 & ~CEC_C1_CLEAR_RECEIVE_OFF);
    d->rx_frames++;
    if (cec_parse(&m, raw, len)) { say(d, now, "RX bad frame"); return; }
    int op = cec_opcode(&m);
    struct cec_key_event ev[CEC_KEYS_MAX_EVENTS]; unsigned n;
    switch (cec_sm_rx(&d->sm, &m)) {
    case CEC_SM_EV_KEY_PRESSED: {
        uint8_t ui = d->sm.ui_code;
        if (!d->ui_seen || ui != d->last_ui || (uint32_t)(now - d->last_ui_ms) > CEC_KEY_RELEASE_MS) {
            char l[CEC_LOG_LINE]; unsigned k = put_str(l, 0, "UI ");
            k = put_hex(l, k, &ui, 1); k = put_str(l, k, cec_keys_map(ui) ? " key " : " UNMAPPED");
            if (cec_keys_map(ui)) k = put_dec(l, k, cec_keys_map(ui));
            l[k] = 0; say(d, now, l);
        }
        d->ui_seen = 1; d->last_ui = ui; d->last_ui_ms = now;
        n = cec_keys_pressed(&d->keys, ui, now, ev); emit(d, ev, n); break;
    }
    case CEC_SM_EV_KEY_RELEASED: n = cec_keys_released(&d->keys, ev); emit(d, ev, n); break;
    default: if (op != CEC_OP_USER_CONTROL_PRESSED) say_frame(d, now, "RX", &m); break;
    }
}

void cec_drv_task(struct cec_drv *d, uint32_t now) {
    struct cec_key_event ev[CEC_KEYS_MAX_EVENTS];
    if (d->sm.state == CEC_SM_NO_ADDRESS && d->last_state == CEC_SM_NO_ADDRESS && !d->sm.inflight && !d->tx_busy) {
        emit(d, ev, cec_keys_tick(&d->keys, now, ev)); return;       /* nothing to run */
    }
    cec_sm_tick(&d->sm, now);
    if (d->tx_busy && !d->sm.inflight) {                            /* the core gave up on it: cancel the hardware */
        d->wr(d->ctx, CEC_R_CNTRL_1, d->rd(d->ctx, CEC_R_CNTRL_1) & ~CEC_C1_START_XMIT);
        uint32_t c5 = d->rd(d->ctx, CEC_R_CNTRL_5);
        d->wr(d->ctx, CEC_R_CNTRL_5, c5 | CEC_C5_TX_SW_RESET); d->wr(d->ctx, CEC_R_CNTRL_5, c5 & ~CEC_C5_TX_SW_RESET);
        d->tx_busy = 0; say(d, now, "send timed out; transmitter reset");
        claimed_check(d, now);
    }
    if (d->rd(d->ctx, CEC_R_CPU_STATUS) & CEC_CPU_CEC) {
        uint32_t c5 = d->rd(d->ctx, CEC_R_CNTRL_5);
        if (c5 & CEC_C5_RX_CEC_INT) rx_frame(d, now);
        else if (d->tx_busy) tx_done(d, now);
        else d->spurious++;
        d->wr(d->ctx, CEC_R_CPU_CLEAR, CEC_CPU_CEC);
    }
    emit(d, ev, cec_keys_tick(&d->keys, now, ev));
    if (!d->tx_busy && cec_sm_next_tx(&d->sm, &d->tx, &d->tx_id)) {
        d->tx_busy = 1; send_frame(d, &d->tx);
    }
}
