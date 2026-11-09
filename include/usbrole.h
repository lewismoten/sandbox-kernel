/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef USBROLE_H
#define USBROLE_H
#include <stdbool.h>
#include <stdint.h>
/* Debounced USB role decision. Pure logic: no registers, host-testable. */
enum usb_role { USB_ROLE_DEVICE, USB_ROLE_HOST_WANTED };
#define USBROLE_HOST_STABLE_MS 500 /* wanted must hold this long before HOST_WANTED */
struct usbrole { enum usb_role role; bool cand; uint32_t since; };
void usbrole_init(struct usbrole *s);
/* id_grounded: OTG adapter ID pin grounded; enumerated: a Mac data link is up.
   Returns the role. Leaving HOST_WANTED is immediate (device is the safe default). */
enum usb_role usbrole_step(struct usbrole *s,bool id_grounded,bool enumerated,uint32_t now_ms);
/* GOTGCTL.ConIDSts (bit 16): 0 = A-device (ID grounded). */
static inline bool usbrole_id_grounded(uint32_t gotgctl) {return !(gotgctl&(1u<<16));}
#endif
