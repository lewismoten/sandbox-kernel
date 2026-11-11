/* SPDX-License-Identifier: MIT; foreground USB scheduling and IRQ-safe counters. */
#pragma once
#include "usb_wait.h"
#define USBHOST_TASK_BUDGET_US 1000u
/* Leave room for one bounded controller operation/callback before yielding. */
#define USBHOST_WORK_RESERVE_US 250u
static inline bool usbhost_work_allowed(uint32_t begin) {
 return !usb_wait_failed()&&(uint32_t)(usb_wait_now()-begin)<USBHOST_TASK_BUDGET_US-USBHOST_WORK_RESERVE_US;
}
static inline uint32_t usbhost_interval_us(bool high_speed,uint8_t interval) {
 uint32_t us=high_speed?(125u<<(interval?((interval>16?16:interval)-1):0)):(uint32_t)interval*1000u;
 return us<8000u?8000u:us;
}
void usbhost_stats_open(uint8_t device,uint8_t ep,uint32_t interval_us);
void usbhost_stats_close(uint8_t device,uint8_t ep); /* ep=0 closes a device */
void usbhost_stats_submit(uint8_t device,uint8_t ep);
void usbhost_stats_token(uint8_t device,uint8_t ep,bool interrupt,bool split,bool complete);
void usbhost_stats_irq(uint32_t us);
void usbhost_stats_pass(uint32_t us);
void usbhost_stats_task(void); /* foreground; one-second snapshot, five-second UART status */
void usbhost_dwc2_task(uint32_t begin);
