/* SPDX-License-Identifier: MIT; the OS program's serial channel (/dev/serial0, ABI 1.10).
 * A byte stream between the running OS and the host on the other end of the
 * USB CDC link, the same link that carries KEY/AXIS. Pure logic: no hardware.
 * Never stalls the OS: with no reader at the host, or a full buffer, bytes are
 * dropped and counted. Wire framing is in docs/serial.md. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define SERIAL_TX_BYTES 512u   /* OS to host buffer */
#define SERIAL_RX_BYTES 256u   /* host to OS buffer */
#define SERIAL_CHUNK 64u       /* data bytes in one "OS " line, and in one "TX" command */
#define SERIAL_LINE_MAX 272u   /* worst framed line, with its newline and a terminator */
#define SERIAL_NONE 0u
#define SERIAL_USB 1u
struct serial_counters {uint32_t transport,tx_bytes,tx_dropped,rx_bytes,rx_dropped,tx_pending,rx_pending;};
int serial_open(void);                 /* 0, or -EBUSY (one owner at a time) */
void serial_close(void);               /* the owner is done: buffers are emptied */
void serial_set_reader(bool connected);/* USB: a host has the port open; false drops what waits */
/* Owner side. serial_write takes all n bytes (dropping and counting any that do
   not fit, or all of them with no reader). serial_read: count, or -EAGAIN. */
uint32_t serial_write(const uint8_t *p,uint32_t n);
int serial_read(uint8_t *out,uint32_t max);
/* Host side. Returns how many bytes were accepted (the rest are counted dropped). */
uint32_t serial_rx_put(const uint8_t *p,uint32_t n);
/* The next wire line for waiting OS output, with its newline; 0 when none. */
uint32_t serial_tx_line(char out[SERIAL_LINE_MAX]);
void serial_counters(struct serial_counters *c);
const char *serial_transport_name(void); /* "usb" or "none" */
