/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* CEC playback-device state machine: logical address polling and replies.
 * Pure: fed frames and time, hands back frames to send. The hardware layer
 * sends what cec_sm_next_tx gives and reports the outcome with cec_sm_tx_done. */
#ifndef CEC_SM_H
#define CEC_SM_H
#include "cec_msg.h"

#define CEC_SM_QUEUE 6u
#define CEC_SM_TX_TIMEOUT_MS 1000u
#define CEC_SM_POLL_RETRIES 2u
#define CEC_SM_OSD_NAME "Game Maker"

enum cec_sm_state { CEC_SM_POLL, CEC_SM_CLAIMED, CEC_SM_NO_ADDRESS };
enum cec_sm_event { CEC_SM_EV_NONE, CEC_SM_EV_KEY_PRESSED, CEC_SM_EV_KEY_RELEASED };
enum cec_sm_tx_result { CEC_SM_TX_ERROR = -1, CEC_SM_TX_NACK = 0, CEC_SM_TX_ACK = 1, CEC_SM_TX_TIMEOUT = 2 };

struct cec_sm {
    enum cec_sm_state state;
    uint16_t phys;
    uint8_t laddr;                 /* claimed address, 15 when none */
    uint8_t cand;                  /* index into the 4/8/11 candidates */
    uint8_t poll_retries;          /* uncertain outcomes for this candidate */
    uint8_t active;                /* we are the active source */
    uint8_t tentative;             /* claimed after a poll that never finished (bus not ready): unverified */
    uint8_t ui_code;               /* operand of the last Pressed */
    uint8_t inflight;              /* a frame is out for sending */
    uint8_t inflight_retry;        /* it was already retried */
    uint8_t retry_next;            /* the next frame out is that retry */
    uint32_t tx_start_ms, now_ms;
    uint64_t tx_id;                /* never reused during this initialization */
    struct cec_msg cur;            /* the frame in flight */
    struct cec_msg q[CEC_SM_QUEUE];
    uint8_t qn;
};

/* phys 0xFFFF = unknown: no address is claimed. Starts polling at once.
 * Before reinitializing, drain/cancel all callbacks from the previous lifetime. */
void cec_sm_init(struct cec_sm *s, uint16_t phys, uint32_t now_ms);
/* Time passes; retire a stuck send as TIMEOUT (a poll with no ACK claims the address). */
void cec_sm_tick(struct cec_sm *s, uint32_t now_ms);
/* A received frame. Returns a key event for User Control frames. */
enum cec_sm_event cec_sm_rx(struct cec_sm *s, const struct cec_msg *m);
/* 1 with *out and *tx_id set for a new transfer (one at a time), else 0.
 * Save this ID with the transfer, including retries; never read it at callback
 * time from s. IDs are nonzero and never wrap: exhaustion stops new transfers. */
int cec_sm_next_tx(struct cec_sm *s, struct cec_msg *out, uint64_t *tx_id);
/* Report ACK/NACK, TIMEOUT for no completion by the deadline, or ERROR for
 * an uncertain transport failure. Poll NACK and TIMEOUT claim an address. Errors retry the same
 * candidate up to CEC_SM_POLL_RETRIES times, then leave us unregistered.
 * For a broadcast, ACK means nobody NACKed. Only the current transfer ID is accepted;
 * timed-out, duplicate and other mismatched completions have no effect. */
void cec_sm_tx_done(struct cec_sm *s, uint64_t tx_id, enum cec_sm_tx_result result);
/* Wake the TV and take the screen: Image View On, then Active Source. */
int cec_sm_standby(struct cec_sm *s);
void cec_sm_announce(struct cec_sm *s);
#endif
