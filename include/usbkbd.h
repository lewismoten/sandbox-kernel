/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef USBKBD_H
#define USBKBD_H
#define USBKBD_MAX 4 /* keyboards at once, on any device behind a hub */
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
/* A HID interface (device address, interface index) mounted. Returns true when it is the boot
 * keyboard the caller should poll; mice, consumer controls and a second keyboard return false.
 * Only the polled interface's reports count, and only its unmount removes the keyboard. */
bool usbkbd_hid_mounted(uint8_t daddr,uint8_t idx,bool boot_keyboard);
/* A report arrived. Returns true when it came from the polled keyboard and it should be re-armed. */
bool usbkbd_report(uint8_t daddr,uint8_t idx,const uint8_t *rep,uint32_t len);
void usbkbd_hid_unmounted(uint8_t daddr,uint8_t idx); /* that interface left: keys released, device removed */
void usbkbd_device_gone(uint8_t daddr);         /* the whole USB device left: its keyboards go, whichever interface */
void usbkbd_poll_failed(uint8_t daddr,uint8_t idx); /* the endpoint could not be (re)armed: remove that keyboard */
unsigned usbkbd_count(void);                    /* boot keyboards ready now, on any device */
bool usbkbd_ready(void);
const char *usbkbd_status(void);                /* short text for the e-ink status line */
/* Name for a HID usage ("A", "Space", "Left"), or NULL when it has none. */
const char *usbkbd_usage_name(uint8_t usage);
#endif
