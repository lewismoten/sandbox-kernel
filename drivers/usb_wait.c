/* SPDX-License-Identifier: MIT */
#include "kernel.h"
#include "usb_wait.h"
#include "usblog.h"
#include "power.h"
static volatile bool failed;
static volatile uint32_t counts[USB_WAIT_COUNT];
static uint32_t reported[USB_WAIT_COUNT];
uint32_t usb_wait_now(void) {return micros();}
bool usb_wait_failed(void) {return failed;}
void usb_wait_timeout(enum usb_wait_kind kind) {
 if(kind<USB_WAIT_COUNT) {if(counts[kind]!=UINT32_MAX) counts[kind]++;}
 failed=true;
}
uint32_t usb_wait_count(enum usb_wait_kind kind) {return kind<USB_WAIT_COUNT?counts[kind]:0;}
bool usb_wait_reg(volatile const uint32_t *reg,uint32_t mask,uint32_t value,uint32_t limit,enum usb_wait_kind kind) {
 if(failed) return false;
 uint32_t start=micros();
 while((*reg&mask)!=value) {
  uint32_t elapsed=(uint32_t)(micros()-start);
  if(elapsed>=limit) {usb_wait_timeout(kind);return false;}
  /* Long limits are boot-only (host mode debounce, core reset): sleep,
   * rather than holding the CPU for the whole deadline. */
  if(limit>=1000u) {
   uint32_t left=limit-elapsed;idle_wait(left<1000u?left:1000u);
  }
 }
 return true;
}
void usb_wait_report(void) {
 static const char *const names[USB_WAIT_COUNT]={"AHB idle","core reset","reset done","TX flush","RX flush",
  "host mode","request queue","RX IRQ drain","IN NAK","IN disable","OUT NAK","OUT disable","semaphore","delay"};
 for(unsigned i=0;i<USB_WAIT_COUNT;i++) {
  uint32_t n=counts[i];if(n==reported[i]) continue;reported[i]=n;
  char line[USBLOG_LINE]="DWC2 timeout ";unsigned at=13;
  for(const char *p=names[i];*p;p++)line[at++]=*p;
  const char *s=" count=";while(*s)line[at++]=*s++;
  char digits[11];unsigned k=0;do{digits[k++]=(char)('0'+n%10);n/=10;}while(n);
  while(k)line[at++]=digits[--k];line[at]=0;usblog_add_critical(line);
 }
}
/* The cooperative stack uses asynchronous timers. Reject unexpected long blocking
 * delays; a void TinyUSB API reports this through the same terminal fault latch. */
void tusb_time_delay_ms_api(uint32_t ms) {
 if(ms>1) {usb_wait_timeout(USB_WAIT_DELAY);return;}
 uint32_t start=micros();while((uint32_t)(micros()-start)<ms*1000u) {}
}
