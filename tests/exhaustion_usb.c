/* SPDX-License-Identifier: MIT; real USB queues, fake bounded CDC transport.
 * Include the implementation so tests observe its private queue accounting
 * without production hooks or changes to the shared host harness. */
#include <assert.h>
#include "../drivers/usb.c"
static bool mounted=true,connected=true,busy;
static unsigned credits,sent,tasks;
static char wire[8192];
volatile bool usb_reset_seen;
const char *property_fail="fake";
bool property(uint32_t t,uint32_t *p,unsigned n) {(void)t;(void)p;(void)n;return false;}
uint32_t micros(void) {return 0;}
void error(const char *s) {(void)s;assert(!"unexpected reply queue overflow");}
void log_line(const char *s) {usb_log(s);}
void boot_summary(void) {}
void protocol_task(void) {}
void protocol_reset(void) {}
bool protocol_busy(void) {return busy;}
int tud_speed_get(void) {return 0;}
void tud_int_handler(unsigned p) {(void)p;}
bool tud_mounted(void) {return mounted;}
bool tud_cdc_connected(void) {return connected;}
bool tusb_init(void) {return true;}
uint32_t tud_cdc_write_available(void) {return credits;}
void tud_task(void) {tasks++;}
void tud_cdc_write_char(char c) {
 assert(credits && sent+1<sizeof wire); credits--; wire[sent++]=c;wire[sent]=0;
}
void tud_cdc_write_str(const char *s) {while(*s)tud_cdc_write_char(*s++);}
void tud_cdc_write_flush(void) {}
static void drain(unsigned bytes) {sent=0;wire[0]=0;credits=bytes;usb_task();}
int main(void) {
 initialized=true;
 for(unsigned cycle=0;cycle<4;cycle++) {
  assert(lw==lr && wp==rp && dropped==0);
  /* A 123-byte message + LOG prefix + LF takes exactly 128 bytes. */
  char line[124];memset(line,'L',sizeof(line)-1);line[123]=0;
  for(unsigned i=0;i<32;i++)usb_log(line);
  assert(lw-lr==4096 && dropped==0);
  char saved[4096];memcpy(saved,logs,sizeof saved);
  unsigned read=lr,write=lw;
  usb_log(line); /* next operation must fail/drop atomically */
  assert(dropped==1 && lr==read && lw==write);
  assert(!memcmp(saved,logs,sizeof saved));
  for(unsigned i=0;i<1024;i++)usb_log(line);
  assert(dropped==1025 && lw-lr==4096);
  assert(!memcmp(saved,logs,sizeof saved));
  /* Stalled reader: service still returns; no host allocation or sleeps. */
  unsigned previous_tasks=tasks;
  for(unsigned i=0;i<32;i++)drain(0);
  assert(tasks==previous_tasks+32 && lr==read && lw==write);
  /* Recovery replies have an independent reserve and priority over logs. */
  usb_reply("OK recovery\n");
  assert(usb_reply_pending());
  busy=true;drain(12);
  assert(sent==12 && !memcmp(wire,"OK recovery\n",12));
  assert(rp==wp && lr==read && lw==write);
  busy=false;
  connected=false;drain(64);assert(sent==0 && lr==read);
  connected=true;
  /* Slow one-byte drains, then exactly one dropped-lines notice. */
  for(unsigned i=0;i<4096;i++) {
   drain(1);assert(sent==1);
   assert(wire[0]==saved[(read+i)%sizeof logs]);
  }
  assert(lr==lw && dropped==1025);
  drain(64);
  assert(!strcmp(wire,"LOG dropped (slow reader)\n") && dropped==0);
  drain(64);assert(sent==0);
  usb_log("after flood");drain(64);
  assert(!strcmp(wire,"LOG after flood\n") && lr==lw && dropped==0);
 }
 return 0;
}
