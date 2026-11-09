/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* CEC playback-device state machine: logical address polling and replies.
 * Pure: fed frames and time, hands back frames to send. The hardware layer
 * sends what cec_sm_next_tx gives and reports the outcome with cec_sm_tx_done. */
#ifndef CEC_SM_H
#define CEC_SM_H
#include "cec_msg.h"

#define CEC_SM_QUEUE 6u
#define CEC_SM_TX_TIMEOUT_MS 1000u
#define CEC_SM_OSD_NAME "Game-Maker"

enum cec_sm_state { CEC_SM_POLL, CEC_SM_CLAIMED, CEC_SM_NO_ADDRESS };
enum cec_sm_event { CEC_SM_EV_NONE, CEC_SM_EV_KEY_PRESSED, CEC_SM_EV_KEY_RELEASED };

struct cec_sm {
    enum cec_sm_state state;
    uint16_t phys;
    uint8_t laddr;                 /* claimed address, 15 when none */
    uint8_t cand;                  /* index into the 4/8/11 candidates */
    uint8_t active;                /* we are the active source */
    uint8_t ui_code;               /* operand of the last Pressed */
    uint8_t inflight;              /* a frame is out for sending */
    uint8_t inflight_retry;        /* it was already retried */
    uint8_t retry_next;            /* the next frame out is that retry */
    uint32_t tx_start_ms, now_ms;
    struct cec_msg cur;            /* the frame in flight */
    struct cec_msg q[CEC_SM_QUEUE];
    uint8_t qn;
};

/* phys 0xFFFF = unknown: no address is claimed. Starts polling at once. */
void cec_sm_init(struct cec_sm *s, uint16_t phys, uint32_t now_ms);
/* Time passes; a stuck send counts as a NACK. */
void cec_sm_tick(struct cec_sm *s, uint32_t now_ms);
/* A received frame. Returns a key event for User Control frames. */
enum cec_sm_event cec_sm_rx(struct cec_sm *s, const struct cec_msg *m);
/* 1 and *out set when a frame should be sent now (one at a time), else 0. */
int cec_sm_next_tx(struct cec_sm *s, struct cec_msg *out);
/* Outcome of the frame handed out: ok = the destination ACKed it. For a
 * broadcast, ok means nobody NACKed. */
void cec_sm_tx_done(struct cec_sm *s, int ok);
/* Wake the TV and take the screen: Image View On, then Active Source. */
void cec_sm_announce(struct cec_sm *s);
#endif
