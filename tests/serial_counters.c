/* SPDX-License-Identifier: MIT; seed cumulative counters at unreachable-in-test-time limits. */
#include "../core/serial.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
 uint8_t b[SERIAL_TX_BYTES+2]={0};struct serial_counters c;
 serial_open();serial_set_reader(true);
 tx_total=rx_total=UINT32_MAX;tx_drop=rx_drop=UINT32_MAX;
 assert(serial_write(b,2)==2);assert(serial_rx_put(b,2)==2);
 serial_set_reader(false);serial_close();assert(!serial_rx_put(b,2));
 serial_counters(&c);
 assert(c.tx_bytes==(uint64_t)UINT32_MAX+2&&c.rx_bytes==(uint64_t)UINT32_MAX+2);
 assert(c.tx_dropped==(uint64_t)UINT32_MAX+2&&c.rx_dropped==(uint64_t)UINT32_MAX+2);
 /* Each accounting path saturates rather than wrapping, and keeps doing work. */
 tx_total=rx_total=tx_drop=rx_drop=UINT64_MAX-1;
 serial_write(NULL,2);serial_write(NULL,2); /* disconnected */
 serial_open();serial_set_reader(true);
 serial_write(b,sizeof(b)); /* accepted bytes and queue-full drops */
 serial_set_reader(false); /* pending drops on disconnect */
 serial_rx_put(b,sizeof(b)); /* accepted bytes and RX-full drops */
 serial_counters(&c);
 assert(c.tx_bytes==UINT64_MAX&&c.tx_dropped==UINT64_MAX);
 assert(c.rx_bytes==UINT64_MAX&&c.rx_dropped==UINT64_MAX);
 assert(c.tx_pending==0&&c.rx_pending==SERIAL_RX_BYTES);
 /* Ring positions still wrap independently of the cumulative counters. */
 tw=tr=rw=rr=UINT32_MAX;
 serial_set_reader(true);assert(serial_write(b,2)==2);assert(serial_rx_put(b,2)==2);
 assert(tw==1&&rw==1);assert(serial_read(b,2)==2);
 serial_counters(&c);assert(c.tx_pending==2&&c.rx_pending==0);
 assert(c.tx_bytes==UINT64_MAX&&c.rx_bytes==UINT64_MAX);
 puts("serial counter boundaries ok");
}
