/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef USBKBD_H
#define USBKBD_H
#include <stdint.h>
#include <stdbool.h>
/* Glue between the USB host stack's callbacks and the input queue (hidboot). No TinyUSB or
 * register code, so the host tests drive it with fakes. Status goes out through log_line
 * (HDMI console) in plain stages: "USB host up", "device attached VID:PID", "keyboard ready",
 * and "key NAME" for each press. */
void usbkbd_init(void);                         /* a fresh start; no keyboard */
void usbkbd_host_up(void);                      /* the stack started */
void usbkbd_attached(uint16_t vid,uint16_t pid);
void usbkbd_detached(void);                     /* the device left the bus */
/* A HID interface mounted. Returns true when it is a boot keyboard the caller should poll. */
bool usbkbd_hid_mounted(bool boot_keyboard);
void usbkbd_report(const uint8_t *rep,uint32_t len);
void usbkbd_hid_unmounted(void);                /* releases every key and removes the device */
bool usbkbd_ready(void);
const char *usbkbd_status(void);                /* short text for the e-ink status line */
/* Name for a HID usage ("A", "Space", "Left"), or NULL when it has none. */
const char *usbkbd_usage_name(uint8_t usage);
#endif
