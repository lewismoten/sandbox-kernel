/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef USBROLE_H
#define USBROLE_H
#include <stdbool.h>
#include <stdint.h>
/* Debounced USB role decision. Pure logic: no registers, host-testable. */
enum usb_role { USB_ROLE_DEVICE, USB_ROLE_HOST_WANTED };
#define USBROLE_HOST_STABLE_US 500000u /* wanted must hold this long (us) before HOST_WANTED */
struct usbrole { enum usb_role role; bool cand; uint32_t since; };
#define USBROLE_GOTGCTL 0x20980000u
#define USBROLE_GUSBCFG 0x2098000cu
#define USBROLE_HPRT0   0x20980440u
#define USBROLE_FORCE_HST (1u<<29)
#define USBROLE_FORCE_DEV (1u<<30)
#define USBROLE_PRTPWR    (1u<<12)
void usbrole_init(struct usbrole *s);
/* id_grounded: OTG adapter ID pin grounded; enumerated: a Mac data link is up.
   now_us is the free-running 32-bit micros() value: it wraps every 2^32 us (71.6 min) and
   only unsigned deltas are used, so the wrap is harmless. Returns the role. Leaving HOST_WANTED is immediate (device is the safe default). */
enum usb_role usbrole_step(struct usbrole *s,bool id_grounded,bool enumerated,uint32_t now_us);
/* GOTGCTL.ConIDSts (bit 16): 0 = A-device (ID grounded). */
static inline bool usbrole_id_grounded(uint32_t gotgctl) {return !(gotgctl&(1u<<16));}
/* The ID sampling window (R1): both force bits clear for up to 100 ms. It opens only if
   the host port is unpowered (HPRT0.PrtPwr clear), and the window never sets any host bit. */
static inline bool usbrole_window_may_open(uint32_t hprt0) {return !(hprt0&USBROLE_PRTPWR);}
static inline uint32_t usbrole_window_gusbcfg(uint32_t gusbcfg) {return gusbcfg&~(USBROLE_FORCE_HST|USBROLE_FORCE_DEV);}
/* GUSBCFG after the window: device forced, host force never set. */
static inline uint32_t usbrole_device_gusbcfg(uint32_t gusbcfg) {return (gusbcfg&~USBROLE_FORCE_HST)|USBROLE_FORCE_DEV;}
#endif
