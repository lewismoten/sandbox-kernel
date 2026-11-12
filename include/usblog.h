/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef USBLOG_H
#define USBLOG_H
#include <stdbool.h>
#include <stdint.h>
/* Host role: a short text record of what the USB stack found and what the kernel did with it, kept in
   RAM (bounded) and saved to /OS/SAVES/KERNEL/USB.TXT after coalescing updates for 1.5 s (core/main.c).
   Pure text logic, host-tested; the driver (drivers/usb_host.c) passes plain numbers. */
#define USBLOG_MAX 8192 /* bytes kept; later lines are dropped after one "log full" line */
#define USBLOG_LINE 96
#define USBLOG_STATS_MAX 2048 /* recent complete one-second windows, reserved from USBLOG_MAX */
void usblog_set_stats(const char *text);
void usblog_clear(void);
void usblog_add_critical(const char *line); /* retain a terminal fault even after log full */
void usblog_add(const char *line);            /* appends line + '\n' */
const char *usblog_text(uint32_t *bytes);
uint32_t usblog_version(void);                /* changes whenever text is added */
/* Writes "<ms> " (ms = whole milliseconds of the 64-bit microsecond clock hi:lo, decimal, no 64-bit divide helper) and returns its length; out holds 22 bytes. */
unsigned usblog_stamp(char *out,uint32_t hi,uint32_t lo);
/* One line each, built from numbers (speed is "LS", "FS" or "HS"; hub 0 = the OTG port itself). */
void usblog_fmt_device(char *out,uint8_t daddr,uint8_t hub,uint8_t port,const char *speed,bool split,
                       uint16_t vid,uint16_t pid,uint8_t cls,uint8_t sub,uint8_t proto,uint8_t ep0);
void usblog_fmt_interface(char *out,uint8_t daddr,uint8_t num,uint8_t cls,uint8_t sub,uint8_t proto,uint8_t neps);
void usblog_fmt_endpoint(char *out,uint8_t daddr,uint8_t num,uint8_t addr,uint8_t attr,uint16_t mps,uint8_t interval);
void usblog_fmt_note(char *out,uint8_t daddr,uint8_t num,const char *text); /* "D2 I0 text"; num 0xFF = device only */
void usblog_fmt_control(char *out,uint8_t d,uint8_t type,uint8_t req,uint16_t value,uint16_t index,uint16_t length,unsigned retry,unsigned delay_ms,const char *outcome);
void usblog_fmt_port_retry(char *out,uint8_t hub,uint8_t port,unsigned retry,const char *outcome);
void usblog_fmt_poll_retry(char *out,uint8_t hub,unsigned retry,unsigned delay_ms,const char *outcome);
#endif
