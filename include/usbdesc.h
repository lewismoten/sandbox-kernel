/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef USBDESC_H
#define USBDESC_H
#include <stdint.h>
/* USB configuration-descriptor walker. Pure logic: no registers, host-testable.
 * Finds the HID boot-protocol keyboard (class 3, subclass 1, protocol 1) on a
 * composite device, in any interface order, with its interrupt IN endpoint. */
#define USBDESC_OK 0
#define USBDESC_E_MALFORMED (-1)  /* bad length, overrun, truncated, wrong type */
#define USBDESC_E_NOT_FOUND (-2)  /* well formed, but no usable boot keyboard */
struct usbdesc_kbd {
 uint8_t config_value;   /* bConfigurationValue for SET_CONFIGURATION */
 uint8_t interface_num;  /* bInterfaceNumber */
 uint8_t endpoint;       /* bEndpointAddress of the interrupt IN endpoint (bit 7 set) */
 uint8_t interval;       /* bInterval (frames at low/full speed) */
 uint16_t max_packet;    /* wMaxPacketSize, at least 8 */
};
/* buf holds the whole configuration descriptor as read (len bytes). out is written only on USBDESC_OK. */
int usbdesc_find_boot_keyboard(const uint8_t *buf,uint32_t len,struct usbdesc_kbd *out);
#endif
