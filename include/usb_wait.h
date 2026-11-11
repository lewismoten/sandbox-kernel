/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Hardware polls use the free-running system timer, including with IRQs masked.
 * A timeout is terminal for this boot: never retry a wedged controller in an ISR. */
enum usb_wait_kind {
 USB_WAIT_AHB, USB_WAIT_RESET, USB_WAIT_RESET_DONE, USB_WAIT_TX_FLUSH,
 USB_WAIT_RX_FLUSH, USB_WAIT_HOST_MODE, USB_WAIT_REQUEST, USB_WAIT_RX_IRQ,
 USB_WAIT_IN_NAK, USB_WAIT_IN_DISABLE, USB_WAIT_OUT_NAK, USB_WAIT_OUT_DISABLE,
 USB_WAIT_SEMAPHORE, USB_WAIT_DELAY, USB_WAIT_COUNT
};
#define USB_WAIT_REG_US 100u
/* Boot-only init waits (AHB idle, core soft reset, reset done, whole-FIFO flush in
 * core/host/device init) can take milliseconds; Linux's dwc2 allows ~10 ms each.
 * Limits of 1 ms or more sleep in 1 ms slices; runtime waits keep USB_WAIT_REG_US. */
#define USB_WAIT_INIT_US 20000u
/* Force-mode debounce can take 100 ms; this boot-only wait sleeps in 1 ms slices. */
#define USB_WAIT_MODE_US 150000u
#define USB_WAIT_IRQ_US 250u
uint32_t usb_wait_now(void);
bool usb_wait_failed(void);
void usb_wait_timeout(enum usb_wait_kind kind);
bool usb_wait_reg(volatile const uint32_t *reg,uint32_t mask,uint32_t value,
                  uint32_t limit,enum usb_wait_kind kind);
uint32_t usb_wait_count(enum usb_wait_kind kind);
void usb_wait_report(void); /* foreground only; counters are safe to record in IRQ */
