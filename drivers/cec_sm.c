/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* CEC playback-device state machine (host-testable, no hardware). */
#include "cec_sm.h"

static const uint8_t cands[3] = { CEC_ADDR_PLAYBACK_1, CEC_ADDR_PLAYBACK_2, CEC_ADDR_PLAYBACK_3 };

static void push(struct cec_sm *s, const struct cec_msg *m) {
    if (s->qn < CEC_SM_QUEUE) s->q[s->qn++] = *m;
}
static void poll_next(struct cec_sm *s) {
    struct cec_msg m;
    cec_build_poll(&m, cands[s->cand]);
    push(s, &m);
}
static void report(struct cec_sm *s) {
    struct cec_msg m;
    cec_build_report_physical_address(&m, s->laddr, s->phys, CEC_DEVTYPE_PLAYBACK);
    push(s, &m);
}
static void active_source(struct cec_sm *s) {
    struct cec_msg m;
    cec_build_active_source(&m, s->laddr, s->phys);
    push(s, &m);
}

void cec_sm_init(struct cec_sm *s, uint16_t phys, uint32_t now_ms) {
    s->phys = phys; s->laddr = CEC_ADDR_BROADCAST; s->cand = 0; s->active = 0;
    s->poll_retries = 0;
    s->ui_code = 0; s->inflight = 0; s->inflight_retry = 0; s->retry_next = 0;
    s->tx_id = 0; s->tx_start_ms = 0; s->now_ms = now_ms; s->qn = 0;
    if (phys == CEC_PHYS_UNKNOWN) { s->state = CEC_SM_NO_ADDRESS; return; }
    s->state = CEC_SM_POLL;
    poll_next(s);
}

void cec_sm_tick(struct cec_sm *s, uint32_t now_ms) {
    s->now_ms = now_ms;
    if (s->inflight && (uint32_t)(now_ms - s->tx_start_ms) >= CEC_SM_TX_TIMEOUT_MS) cec_sm_tx_done(s, s->tx_id, CEC_SM_TX_TIMEOUT);
}

int cec_sm_next_tx(struct cec_sm *s, struct cec_msg *out, uint64_t *tx_id) {
    if (s->inflight || s->qn == 0 || s->tx_id == UINT64_MAX) return 0;
    s->cur = s->q[0];
    for (unsigned i = 1; i < s->qn; i++) s->q[i - 1] = s->q[i];
    s->qn--;
    s->inflight = 1; s->inflight_retry = s->retry_next; s->retry_next = 0; s->tx_start_ms = s->now_ms;
    *tx_id = ++s->tx_id;
    *out = s->cur;
    return 1;
}

void cec_sm_tx_done(struct cec_sm *s, uint64_t tx_id, enum cec_sm_tx_result result) {
    if (!s->inflight || tx_id != s->tx_id) return;
    int was_poll = cec_is_poll(&s->cur);
    s->inflight = 0;
    if (was_poll && s->state == CEC_SM_POLL) {
        if (result == CEC_SM_TX_ACK) {               /* somebody owns it, try the next */
            s->poll_retries = 0;
            if (++s->cand >= 3u) { s->state = CEC_SM_NO_ADDRESS; s->laddr = CEC_ADDR_BROADCAST; }
            else poll_next(s);
        } else if (result == CEC_SM_TX_NACK || result == CEC_SM_TX_TIMEOUT) { /* no ACK: free */
            struct cec_msg m;
            s->laddr = cands[s->cand]; s->state = CEC_SM_CLAIMED;
            report(s);
            cec_build_set_osd_name(&m, s->laddr, CEC_ADDR_BROADCAST, CEC_SM_OSD_NAME);
            push(s, &m);
        } else if (s->poll_retries < CEC_SM_POLL_RETRIES) {
            s->poll_retries++;
            poll_next(s);                           /* unknown outcome: same candidate */
        } else {
            s->state = CEC_SM_NO_ADDRESS;
            s->laddr = CEC_ADDR_BROADCAST;
        }
        return;
    }
    if (result != CEC_SM_TX_ACK && !s->inflight_retry) { /* one retry of a failed frame, sent first */
        if (s->qn < CEC_SM_QUEUE) {
            for (unsigned i = s->qn; i > 0; i--) s->q[i] = s->q[i - 1];
            s->q[0] = s->cur; s->qn++;
            s->retry_next = 1;                      /* so a second failure is final */
        }
    }
}

int cec_sm_standby(struct cec_sm *s) {
 if(s->state!=CEC_SM_CLAIMED||s->qn==CEC_SM_QUEUE) return -1;
 struct cec_msg m={.len=2,.b={(uint8_t)(s->laddr<<4),CEC_OP_STANDBY}};
 s->active=0;push(s,&m);return 0;
}

void cec_sm_announce(struct cec_sm *s) {
    struct cec_msg m;
    if (s->state != CEC_SM_CLAIMED) return;
    s->active = 1;
    cec_build_image_view_on(&m, s->laddr, CEC_ADDR_TV);
    push(s, &m);
    active_source(s);
}

enum cec_sm_event cec_sm_rx(struct cec_sm *s, const struct cec_msg *m) {
    struct cec_msg r;
    unsigned from = cec_initiator(m);
    int op = cec_opcode(m);
    if (s->state != CEC_SM_CLAIMED || op < 0) return CEC_SM_EV_NONE;
    if (from == CEC_ADDR_BROADCAST) return CEC_SM_EV_NONE;
    if (cec_is_broadcast(m)) {
        if (op == CEC_OP_REQUEST_ACTIVE_SOURCE) { if (s->active) active_source(s); }
        else if (op == CEC_OP_SET_STREAM_PATH && cec_nparams(m) >= 2) {
            const uint8_t *p = cec_params(m);
            if (((uint16_t)(p[0] << 8 | p[1])) == s->phys) { s->active = 1; active_source(s); }
            else s->active = 0;
        }
        return CEC_SM_EV_NONE;                       /* other broadcasts: silence */
    }
    if (cec_dest(m) != s->laddr) return CEC_SM_EV_NONE;
    switch (op) {
    case CEC_OP_USER_CONTROL_PRESSED:
        if (cec_nparams(m) >= 1) { s->ui_code = cec_params(m)[0]; return CEC_SM_EV_KEY_PRESSED; }
        return CEC_SM_EV_NONE;
    case CEC_OP_USER_CONTROL_RELEASED: return CEC_SM_EV_KEY_RELEASED;
    case CEC_OP_GIVE_PHYSICAL_ADDR: report(s); break;
    case CEC_OP_GIVE_OSD_NAME:
        cec_build_set_osd_name(&r, s->laddr, from, CEC_SM_OSD_NAME); push(s, &r); break;
    case CEC_OP_GIVE_POWER_STATUS:
        cec_build_report_power_status(&r, s->laddr, from, CEC_POWER_ON); push(s, &r); break;
    case CEC_OP_GET_CEC_VERSION:
        cec_build_cec_version(&r, s->laddr, from, CEC_VERSION_1_4); push(s, &r); break;
    case CEC_OP_MENU_REQUEST:                        /* we show no menu for the TV to drive */
        cec_build_menu_status(&r, s->laddr, from, CEC_MENU_DEACTIVATED); push(s, &r); break;
    case CEC_OP_FEATURE_ABORT: break;                /* never abort an abort */
    default:                                         /* Give Device Vendor ID and the rest */
        cec_build_feature_abort(&r, s->laddr, from, (uint8_t)op, CEC_ABORT_UNRECOGNIZED); push(s, &r);
    }
    return CEC_SM_EV_NONE;
}
