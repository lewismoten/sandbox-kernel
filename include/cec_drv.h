/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). The CEC transport: drives the BCM2835 HDMI block's CEC
 * registers through a read/write pair (the real registers in drivers/cec_hw.c, a fake in the
 * host test), runs cec_sm and cec_keys, and logs. Polled from the main loop; no interrupts. */
#ifndef CEC_DRV_H
#define CEC_DRV_H
#include "cec_sm.h"
#include "cec_keys.h"
#include "cec_log.h"
#include "cec_regs.h"

struct cec_drv {
    uint32_t (*rd)(void *ctx, uint32_t off);
    void (*wr)(void *ctx, uint32_t off, uint32_t v);
    void (*key)(void *ctx, uint16_t code, uint8_t value); /* value 0 up, 1 down, 2 repeat */
    void (*wake)(void *ctx); /* accepted button / display activation */
    int (*phys)(void *ctx);  /* optional: read the display's physical address again (EDID); -1 if unreadable */
    void *ctx;
    struct cec_sm sm;
    struct cec_keys keys;
    struct cec_log log;
    struct cec_msg tx;            /* the frame the hardware is sending */
    uint64_t tx_id;
    uint8_t tx_busy, announced, last_state, last_ui, ui_seen;
    uint32_t rx_frames, tx_frames, tx_good, tx_bad, key_events, spurious;
    uint32_t last_ui_ms;
    uint32_t hold_until_ms;       /* no new send before this (after a failed one) */
    uint8_t hold, diag;
    struct cec_timing t;          /* what the block was programmed with */
    uint32_t hb_tx, hb_rx;
    uint32_t alive_ms;            /* last heartbeat line */
    uint32_t check_ms;            /* last register check */
    uint8_t bus_ok;               /* some transfer has completed (ACK, NACK or a received frame) since the last claim */
    uint8_t hpd, hpd_known, phys_assumed, phys_want, phys_failed, retry_armed;
    uint32_t backoff_ms, retry_ms, phys_try_ms, restarts, hpd_rises;
    uint8_t tx_eom_stale, rx_flag_bad, rx_flag_off;  /* flag-based completion/receive detection (see cec_drv.c) */
    uint32_t flags_sig, flags_ms, rx_flag_ms, rx_via_flags;
    uint32_t reprogrammed;        /* times the registers were found changed and written again */
};

/* Program the block (divider from hsm_hz, thresholds, unregistered address, receive reset) and
 * start claiming a logical address for physical address phys (0xFFFF: none, stays silent).
 * Set rd, wr, key and ctx first. */
void cec_drv_init(struct cec_drv *d, uint16_t phys, uint32_t hsm_hz, uint32_t now_ms);
/* One pass: service a pending event before expiring a stuck send, release held
 * keys, and start the next queued frame after any cancellation hold-off. */
void cec_drv_task(struct cec_drv *d, uint32_t now_ms);
#endif
