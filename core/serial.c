/* SPDX-License-Identifier: MIT; the OS serial channel (see include/serial.h, docs/serial.md). */
#include "serial.h"
static uint8_t tx[SERIAL_TX_BYTES],rx[SERIAL_RX_BYTES];
static uint32_t tw,tr,rw,rr,tx_total,tx_drop,rx_total,rx_drop;
static bool owned,reader;
static void forget(void) {tr=tw;rr=rw;}
int serial_open(void) {if(owned)return -16;owned=true;forget();return 0;}
void serial_close(void) {owned=false;forget();}
void serial_set_reader(bool c) {if(c==reader)return;reader=c;if(!c){tx_drop+=tw-tr;tr=tw;}}
uint32_t serial_write(const uint8_t *p,uint32_t n) {
 if(!reader) {tx_drop+=n;return n;}
 for(uint32_t i=0;i<n;i++) {
  if(tw-tr>=SERIAL_TX_BYTES) {tx_drop+=n-i;break;}
  tx[tw++%SERIAL_TX_BYTES]=p[i];tx_total++;
 }
 return n;
}
int serial_read(uint8_t *out,uint32_t max) {
 if(rr==rw)return -11;
 uint32_t n=0;while(n<max&&rr!=rw)out[n++]=rx[rr++%SERIAL_RX_BYTES];
 return (int)n;
}
uint32_t serial_rx_put(const uint8_t *p,uint32_t n) {
 uint32_t a=0;
 if(owned) while(a<n&&rw-rr<SERIAL_RX_BYTES) {rx[rw++%SERIAL_RX_BYTES]=p[a++];}
 rx_total+=a;rx_drop+=n-a;return a;
}
/* One line per call: "OS <text>\n" when the data had a newline, "OS~ <text>\n"
   when it was cut (no newline yet, or SERIAL_CHUNK bytes) and the next line
   continues it. Printable ASCII is sent as is; backslash, controls and bytes
   above 126 as \\ and \xHH, so a line is always plain text. */
uint32_t serial_tx_line(char out[SERIAL_LINE_MAX]) {
 if(tr==tw)return 0;
 static const char hexd[]="0123456789abcdef";
 uint32_t k=0,taken=0,look=0;bool whole=false;
 while(look<SERIAL_CHUNK&&tr+look!=tw) {if(tx[(tr+look)%SERIAL_TX_BYTES]=='\n') {whole=true;break;}look++;}
 out[k++]='O';out[k++]='S';if(!whole)out[k++]='~';out[k++]=' ';
 while(tr!=tw&&taken<SERIAL_CHUNK) {
  uint8_t c=tx[tr++%SERIAL_TX_BYTES];taken++;
  if(c=='\n') break;
  if(c=='\\') {out[k++]='\\';out[k++]='\\';}
  else if(c>=32&&c<127) out[k++]=(char)c;
  else {out[k++]='\\';out[k++]='x';out[k++]=hexd[c>>4];out[k++]=hexd[c&15];}
 }
 out[k++]='\n';out[k]=0;return k;
}
void serial_counters(struct serial_counters *c) {
 c->transport=reader?SERIAL_USB:SERIAL_NONE;c->tx_bytes=tx_total;c->tx_dropped=tx_drop;
 c->rx_bytes=rx_total;c->rx_dropped=rx_drop;c->tx_pending=tw-tr;c->rx_pending=rw-rr;
}
const char *serial_transport_name(void) {return reader?"usb":"none";}
