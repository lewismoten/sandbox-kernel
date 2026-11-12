/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). CEC transport over the register layout in cec_regs.h.
 * The send and receive sequences follow what Linux's vc4_hdmi.c does (GPL-2.0, facts only; NOTICE.md). */
#include "cec_drv.h"

#define CEC_DRV_ALIVE_MS 30000u  /* a heartbeat line, only if frames moved since the last one */
#define CEC_DRV_HOLD_MS 250u  /* after a failed send: the block may still be on its own retry */
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

static void say_regs(struct cec_drv *d, uint32_t now, const char *tag) {
    char t[CEC_LOG_LINE]; unsigned n = put_str(t, 0, tag);
    static const unsigned offs[3] = { CEC_R_CNTRL_1, CEC_R_CNTRL_5, CEC_R_CPU_STATUS };
    static const char *const nm[3] = { " c1 ", " c5 ", " st " };
    for (unsigned i = 0; i < 3; i++) {
        uint32_t v = d->rd(d->ctx, offs[i]); n = put_str(t, n, nm[i]);
        for (int k = 7; k >= 0; k--) t[n++] = hexd[(v >> (4 * k)) & 15u];
    }
    n = put_str(t, n, " mk "); { uint32_t v = d->rd(d->ctx, CEC_R_CPU_MASK_STATUS); for (int k = 7; k >= 0; k--) t[n++] = hexd[(v >> (4 * k)) & 15u]; }
    n = put_str(t, n, " hpd "); t[n++] = (d->rd(d->ctx, CEC_R_HOTPLUG) & CEC_HOTPLUG_CONNECTED) ? '1' : '0';
    n = put_str(t, n, " hp "); { uint32_t v = d->rd(d->ctx, CEC_R_HOTPLUG); for (int k = 1; k >= 0; k--) t[n++] = hexd[(v >> (4 * k)) & 15u]; }
    n = put_str(t, n, " hi "); { uint32_t v = d->rd(d->ctx, CEC_R_HOTPLUG_INT); for (int k = 1; k >= 0; k--) t[n++] = hexd[(v >> (4 * k)) & 15u]; }
    t[n] = 0; say(d, now, t);
}

static void emit(struct cec_drv *d, const struct cec_key_event *ev, unsigned n) {
    for (unsigned i = 0; i < n; i++) { d->key_events++; if (d->key) d->key(d->ctx, ev[i].code, ev[i].value); }
}

/* Divider, address field, thresholds and a receive/transmit reset: everything the block keeps that a
 * mode change by the firmware can wipe. addr 15 = unregistered. */
static void program(struct cec_drv *d, unsigned addr) {
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);
    c1 &= ~(CEC_C1_DIV_MASK | CEC_C1_ADDR_MASK | CEC_C1_START_XMIT);
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 | ((uint32_t)addr << CEC_C1_ADDR_SHIFT) | d->t.div);
    uint32_t c5 = d->rd(d->ctx, CEC_R_CNTRL_5) & CEC_C5_KEEP_MASK;
    c5 |= d->t.c5;
    d->wr(d->ctx, CEC_R_CNTRL_5, c5 | CEC_C5_TX_SW_RESET | CEC_C5_RX_SW_RESET);
    d->wr(d->ctx, CEC_R_CNTRL_5, c5);
    d->wr(d->ctx, CEC_R_CNTRL_2, d->t.c2); d->wr(d->ctx, CEC_R_CNTRL_3, d->t.c3); d->wr(d->ctx, CEC_R_CNTRL_4, d->t.c4);
    d->wr(d->ctx, CEC_R_CPU_CLEAR, CEC_CPU_CEC);                  /* drop any stale event */
    d->wr(d->ctx, CEC_R_CPU_MASK_CLEAR, CEC_CPU_CEC);             /* Linux unmasks the CEC interrupt on enable; nothing here ever did */
}

/* The firmware reconfigures HDMI when the kernel changes the video mode (GMOS does at start), which
 * has been seen to leave the CEC block unprogrammed. Look every 100 ms; write it all again if the
 * divider or address no longer match, and give up the frame that was on the wire. */
static void claimed_check(struct cec_drv *d, uint32_t now);
static void check_regs(struct cec_drv *d, uint32_t now) {
    if ((uint32_t)(now - d->check_ms) < 100u) return;
    d->check_ms = now;
    d->wr(d->ctx, CEC_R_CPU_MASK_CLEAR, CEC_CPU_CEC);             /* idempotent; the firmware may mask it again */
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);
    unsigned want = d->sm.state == CEC_SM_CLAIMED ? d->sm.laddr : CEC_ADDR_BROADCAST;
    if ((c1 & CEC_C1_DIV_MASK) == d->t.div && ((c1 & CEC_C1_ADDR_MASK) >> CEC_C1_ADDR_SHIFT) == want
        && d->rd(d->ctx, CEC_R_CNTRL_2) == d->t.c2) return;
    say_regs(d, now, "registers changed;");
    program(d, want); d->reprogrammed++;
    if (d->tx_busy) { d->tx_busy = 0; cec_sm_tx_done(&d->sm, d->tx_id, CEC_SM_TX_ERROR); claimed_check(d, now); }
    say(d, now, "registers programmed again");
}

/* Start the claim over (the registers written again, the queue cleared, a fresh poll). Used when the bus
 * did not answer, the display's address changed, or an unverified claim can be checked. Never gives up. */
static void restart_claim(struct cec_drv *d, uint32_t now, const char *why, uint16_t phys) {
    say(d, now, why);
    program(d, CEC_ADDR_BROADCAST);
    d->tx_busy = 0; d->hold = 0; d->tx_id = 0; d->announced = 0; d->bus_ok = 0; d->retry_armed = 0; d->restarts++;
    cec_sm_init(&d->sm, phys, now);
    d->last_state = d->sm.state;
}

/* The display's address may not have been readable at boot (a TV that is still starting), or may change. */
static void phys_refresh(struct cec_drv *d, uint32_t now, const char *why) {
    char l[CEC_LOG_LINE]; unsigned n;
    int p = d->phys ? d->phys(d->ctx) : -1;
    d->phys_try_ms = now;
    if (p < 0) {                                  /* a read that fails (the display still starting) is tried again, every 3 s while it is connected */
        if (d->phys_want || d->phys_failed) { d->phys_want = 1; d->phys_failed = 1; }
        return;
    }
    d->phys_want = 0; d->phys_failed = 0;
    if ((uint16_t)p == d->sm.phys && !d->phys_assumed) return;
    n = put_str(l, 0, "physical address now "); n = put_hex(l, n, (uint8_t[]){(uint8_t)(p >> 8), (uint8_t)p}, 2);
    n = put_str(l, n, " ("); n = put_str(l, n, why); l[n++] = ')'; l[n] = 0;
    d->phys_assumed = 0;
    restart_claim(d, now, l, (uint16_t)p);
}

void cec_drv_init(struct cec_drv *d, uint16_t phys, uint32_t hsm_hz, uint32_t now_ms) {
    struct cec_timing t; char l[CEC_LOG_LINE]; unsigned n;
    d->tx_busy = 0; d->hold = 0; d->announced = 0; d->last_ui = 0; d->ui_seen = 0; d->tx_id = 0;
    d->bus_ok = 0; d->tx_eom_stale = d->rx_flag_bad = d->rx_flag_off = d->rx_int_prev = d->rx_pending = d->last_raw_len = 0;
    d->flags_sig = d->flags_ms = d->rx_flag_ms = d->rx_via_flags = d->last_raw_ms = d->rx_dups = 0;
    d->hpd_known = 0; d->phys_want = 0; d->phys_failed = 0; d->retry_armed = 0; d->backoff_ms = 0; d->restarts = d->hpd_rises = 0; d->phys_try_ms = now_ms;
    d->rx_frames = d->tx_frames = d->tx_good = d->tx_bad = d->key_events = d->spurious = 0;
    cec_log_init(&d->log); cec_keys_init(&d->keys);
    n = put_str(l, 0, "init phys "); n = put_hex(l, n, (uint8_t[]){(uint8_t)(phys >> 8), (uint8_t)phys}, 2);
    n = put_str(l, n, " hsm "); n = put_dec(l, n, hsm_hz); l[n] = 0; say(d, now_ms, l);
    if (cec_regs_timing(hsm_hz, &t)) {
        say(d, now_ms, "no clock divider: CEC off");
        cec_sm_init(&d->sm, CEC_PHYS_UNKNOWN, now_ms); d->last_state = d->sm.state; return;
    }
    n = put_str(l, 0, "divider "); n = put_dec(l, n, t.div); l[n] = 0; say(d, now_ms, l);
    d->t = t; d->check_ms = now_ms; d->alive_ms = now_ms; d->reprogrammed = 0;
    program(d, CEC_ADDR_BROADCAST);
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
    d->tx_eom_stale = (d->rd(d->ctx, CEC_R_CNTRL_1) & CEC_C1_TX_EOM) ? 1u : 0u;   /* an EOM that survived the start is no news */
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
        say(d, now, "no logical address claimed (all polls acked or transport error); silent");
    }
}

/* Pulse CLEAR_RECEIVE_OFF. Linux does it after a received frame only; after a failed directed send it is kept
 * here because the bench runs that worked all had it (docs/cec.md); whether it is needed is not known. */
static void clear_rx_off(struct cec_drv *d) {
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 | CEC_C1_CLEAR_RECEIVE_OFF);
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 & ~CEC_C1_CLEAR_RECEIVE_OFF);
}

static void tx_done(struct cec_drv *d, uint32_t now) {
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);
    int good = (c1 & CEC_C1_TX_STATUS_GOOD) != 0;
    int poll = d->tx.len == 1, bcast = cec_dest(&d->tx) == CEC_ADDR_BROADCAST && !poll;
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 & ~CEC_C1_START_XMIT);
    if (!good && !poll && !bcast) { clear_rx_off(d); d->hold = 1; d->hold_until_ms = now + CEC_DRV_HOLD_MS; }
    d->tx_busy = 0; d->tx_frames++; d->bus_ok = 1; d->backoff_ms = 0; d->retry_armed = 0;
    if (d->tx_frames <= 16u) say_regs(d, now, "done;");   /* the raw status of the first sends, to tell a wire problem from a software one */
    if (good) d->tx_good++; else d->tx_bad++;
    char l[CEC_LOG_LINE]; unsigned n = put_str(l, 0, poll ? "poll " : "TX ");
    if (!poll) { n = put_dec(l, n, cec_initiator(&d->tx)); l[n++] = '>'; }
    n = put_dec(l, n, cec_dest(&d->tx));
    if (d->tx.len > 1) { l[n++] = ' '; n = put_hex(l, n, d->tx.b + 1, (unsigned)d->tx.len - 1u); }
    n = put_str(l, n, good ? " ack" : " nack"); l[n] = 0; say(d, now, l);
    /* A broadcast is acknowledged by silence, so the hardware's flag says little: never retry one. */
    cec_sm_tx_done(&d->sm, d->tx_id, (good || bcast) ? CEC_SM_TX_ACK : CEC_SM_TX_NACK);
    claimed_check(d, now);
}

/* c5 bit 23 is a level that may stay high (a latched cause) after the receive is cleared, so it only counts when it rises:
 * a rise is remembered in rx_pending until a frame has been read. RX_EOM is the hardware's own end-of-frame flag. */
static void rx_note(struct cec_drv *d, uint32_t c5) {
    uint8_t now_hi = (c5 & CEC_C5_RX_CEC_INT) ? 1u : 0u;
    if (now_hi && !d->rx_int_prev) d->rx_pending = 1;
    d->rx_int_prev = now_hi;
}

static int rx_frame(struct cec_drv *d, uint32_t now, const char *via) {
    uint32_t c1 = d->rd(d->ctx, CEC_R_CNTRL_1);
    unsigned len = 1u + ((c1 & CEC_C1_REC_WRD_MASK) >> CEC_C1_REC_WRD_SHIFT);
    uint8_t raw[16]; struct cec_msg m;
    if (len > 16u) len = 16u;
    for (unsigned i = 0; i < len; i += 4) {
        uint32_t w = d->rd(d->ctx, CEC_R_RX_DATA + (i >> 2) * 4u);
        for (unsigned k = 0; k < 4; k++) raw[i + k] = (uint8_t)(w >> (8u * k));
    }
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 | CEC_C1_CLEAR_RECEIVE_OFF);   /* ready for the next frame */
    d->wr(d->ctx, CEC_R_CNTRL_1, c1 & ~CEC_C1_CLEAR_RECEIVE_OFF);
    d->rx_pending = 0;
    unsigned same = len == d->last_raw_len; for (unsigned i = 0; same && i < len; i++) if (raw[i] != d->last_raw[i]) same = 0;
    if (same && (uint32_t)(now - d->last_raw_ms) < 20u) {
        d->rx_dups++; return 1;                                      /* the same frame read again within 20 ms: a stale one, not a new press (a held remote repeats every 100 ms or more) */
    }
    for (unsigned i = 0; i < len; i++) d->last_raw[i] = raw[i];
    d->last_raw_len = (uint8_t)len; d->last_raw_ms = now;
    d->rx_frames++; d->bus_ok = 1; d->backoff_ms = 0; d->retry_armed = 0;
    {   char l[CEC_LOG_LINE]; unsigned n = put_str(l, 0, "rx raw "); n = put_dec(l, n, len); n = put_str(l, n, "B ");
        n = put_hex(l, n, raw, len); n = put_str(l, n, " via "); n = put_str(l, n, via);
        n = put_str(l, n, " c1 "); for (int k = 7; k >= 0; k--) l[n++] = hexd[(c1 >> (4 * k)) & 15u]; l[n] = 0; say(d, now, l); }
    if (cec_parse(&m, raw, len)) { say(d, now, "RX bad frame"); return 0; }
    int op = cec_opcode(&m);
    if(d->wake && d->sm.state==CEC_SM_CLAIMED && cec_initiator(&m)!=CEC_ADDR_BROADCAST) {
        unsigned dst=cec_dest(&m);
        if((dst==d->sm.laddr &&
            ((op==CEC_OP_USER_CONTROL_PRESSED && cec_nparams(&m)>=1) ||
             (op==CEC_OP_IMAGE_VIEW_ON && !cec_nparams(&m)))))
            /* A foreign broadcast Active Source is another device taking the input: it must
               neither wake us nor make us announce and take it back. */
            d->wake(d->ctx);
    }
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
    return 1;
}

void cec_drv_task(struct cec_drv *d, uint32_t now) {
    struct cec_key_event ev[CEC_KEYS_MAX_EVENTS];
    if (d->sm.state == CEC_SM_NO_ADDRESS && d->last_state == CEC_SM_NO_ADDRESS && !d->sm.inflight && !d->tx_busy) {
        emit(d, ev, cec_keys_tick(&d->keys, now, ev)); return;       /* nothing to run */
    }
    /* Consume a latched ACK/NACK before expiring the transfer: main-loop work can
     * delay servicing an event past the software deadline. The CPU status bit is the normal sign;
     * the 2026-11-12 Insignia log showed c5 bit 23 (a receive event) high with the status bit clear, so the
     * flags in c1 and c5 are looked at too, and the raw words are logged when they change. */
    {   uint32_t st = d->rd(d->ctx, CEC_R_CPU_STATUS), c1 = d->rd(d->ctx, CEC_R_CNTRL_1), c5 = d->rd(d->ctx, CEC_R_CNTRL_5);
        uint32_t sig = st ^ (c5 & CEC_C5_RX_CEC_INT) ^ (c1 & (CEC_C1_TX_EOM | CEC_C1_RX_EOM | CEC_C1_TX_STATUS_GOOD));
        if (sig != d->flags_sig && (uint32_t)(now - d->flags_ms) >= 250u) { d->flags_sig = sig; d->flags_ms = now; say_regs(d, now, "flags;"); }
        int tx_flag = d->tx_busy && (c1 & CEC_C1_TX_EOM) && !d->tx_eom_stale;
        rx_note(d, c5);
        int rx_new = d->rx_pending || (c1 & CEC_C1_RX_EOM);
        int rx_flag = !d->rx_flag_off && !d->tx_busy && rx_new && (uint32_t)(now - d->rx_flag_ms) >= 30u;
        if (st & CEC_CPU_CEC) {
            if (rx_new && !(tx_flag && !(c1 & CEC_C1_RX_EOM))) rx_frame(d, now, "status");
            else if (d->tx_busy) tx_done(d, now);
            else { d->spurious++; say_regs(d, now, "idle event;"); }
            d->wr(d->ctx, CEC_R_CPU_CLEAR, CEC_CPU_CEC);
        } else if (tx_flag) {
            say(d, now, "send finished (c1 flag, status bit clear)"); tx_done(d, now);
        } else if (rx_flag) {
            d->rx_flag_ms = now; d->rx_via_flags++;
            if (d->rx_via_flags <= 5u) say_regs(d, now, "receive flag, status bit clear;");
            if (rx_frame(d, now, "flags")) d->rx_flag_bad = 0;
            else if (++d->rx_flag_bad >= 8u) { d->rx_flag_off = 1; say(d, now, "receive flags give no frames: flag path off"); }
            if ((d->rd(d->ctx, CEC_R_CNTRL_1) & CEC_C1_RX_EOM) || (d->rd(d->ctx, CEC_R_CNTRL_5) & CEC_C5_RX_CEC_INT)) {
                if (!d->rx_flag_off) say(d, now, "receive flag stays set after the clear: flag path off");
                d->rx_flag_off = 1;
            }
        }
    }
    cec_sm_tick(&d->sm, now);
    if (d->tx_busy && !d->sm.inflight) {                            /* the core gave up on it: cancel the hardware */
        d->wr(d->ctx, CEC_R_CNTRL_1, d->rd(d->ctx, CEC_R_CNTRL_1) & ~CEC_C1_START_XMIT);
        uint32_t c5 = d->rd(d->ctx, CEC_R_CNTRL_5);
        d->wr(d->ctx, CEC_R_CNTRL_5, c5 | CEC_C5_TX_SW_RESET); d->wr(d->ctx, CEC_R_CNTRL_5, c5 & ~CEC_C5_TX_SW_RESET);
        rx_note(d, d->rd(d->ctx, CEC_R_CNTRL_5));
        if (d->rx_pending || (d->rd(d->ctx, CEC_R_CNTRL_1) & CEC_C1_RX_EOM)) {
            say_regs(d, now, "receiver flagged at timeout;");
            rx_frame(d, now, "timeout");                                /* read it, as the interrupt handler would, and re-arm the receiver */
        }
        d->wr(d->ctx, CEC_R_CPU_CLEAR, CEC_CPU_CEC);
        if (cec_is_poll(&d->tx)) {
            char l[CEC_LOG_LINE]; unsigned n = put_str(l, 0, "poll ");
            n = put_dec(l, n, cec_dest(&d->tx)); n = put_str(l, n, " timeout"); l[n] = 0; say(d, now, l);
        } else say_regs(d, now, "send timed out;");
        d->tx_busy = 0; d->hold = 1; d->hold_until_ms = now + CEC_DRV_HOLD_MS;
        if (!d->bus_ok && !d->retry_armed) {                          /* nothing has ever completed: try again later, longer each time */
            d->backoff_ms = d->backoff_ms ? (d->backoff_ms >= 15000u ? 30000u : d->backoff_ms * 2u) : 2000u;
            d->retry_ms = now + d->backoff_ms; d->retry_armed = 1;
        }
        if (!cec_is_poll(&d->tx)) say(d, now, "transmitter reset");
        claimed_check(d, now);
    }
    check_regs(d, now);
    {   uint8_t h = (d->rd(d->ctx, CEC_R_HOTPLUG) & CEC_HOTPLUG_CONNECTED) ? 1u : 0u;
        if (!d->hpd_known) { d->hpd_known = 1; d->hpd = h; }
        else if (h != d->hpd) {
            say(d, now, h ? "hotplug rose" : "hotplug fell"); d->hpd = h;
            if (h) { d->hpd_rises++; d->phys_want = 1; d->phys_failed = 0; }
            else { d->phys_want = 0; d->phys_failed = 0; }
        }
    }
    if (d->phys && ((d->phys_want && (!d->phys_failed || (uint32_t)(now - d->phys_try_ms) >= 3000u)) || (d->phys_assumed && (uint32_t)(now - d->phys_try_ms) >= 3000u)) && !d->tx_busy)
        phys_refresh(d, now, d->phys_want ? "display connected" : "EDID read again");
    if (d->sm.phys != CEC_PHYS_UNKNOWN && d->sm.state != CEC_SM_NO_ADDRESS && !d->tx_busy && !d->sm.inflight) {
        if (d->bus_ok && d->sm.state == CEC_SM_CLAIMED && d->sm.tentative)
            restart_claim(d, now, "bus answers now: checking the address again", d->sm.phys);
        else if (d->retry_armed && !d->bus_ok && !d->sm.qn && (int32_t)(now - d->retry_ms) >= 0) {
            uint32_t before = d->restarts;
            if (d->phys) phys_refresh(d, now, "retry");                /* a changed address restarts the claim itself */
            if (d->restarts == before) restart_claim(d, now, "no answer from the bus: starting again", d->sm.phys);
        }
    }
    if ((uint32_t)(now - d->alive_ms) >= CEC_DRV_ALIVE_MS && (d->tx_frames != d->hb_tx || d->rx_frames != d->hb_rx)) {              /* proves in the log that the task is being run */
        char l[CEC_LOG_LINE]; unsigned n = put_str(l, 0, "alive tx "); n = put_dec(l, n, d->tx_good); l[n++] = '/'; n = put_dec(l, n, d->tx_frames);
        n = put_str(l, n, " rx "); n = put_dec(l, n, d->rx_frames); l[n] = 0; say(d, now, l); d->hb_tx = d->tx_frames; d->hb_rx = d->rx_frames;
    }
    if ((uint32_t)(now - d->alive_ms) >= CEC_DRV_ALIVE_MS) d->alive_ms = now;
    emit(d, ev, cec_keys_tick(&d->keys, now, ev));
    if (d->hold && (int32_t)(now - d->hold_until_ms) >= 0) d->hold = 0;
    if (!d->tx_busy && !d->hold && cec_sm_next_tx(&d->sm, &d->tx, &d->tx_id)) {
        d->tx_busy = 1; d->diag = 0; send_frame(d, &d->tx); if (!cec_is_poll(&d->tx)) say_regs(d, now, "sent;");
    }
}
