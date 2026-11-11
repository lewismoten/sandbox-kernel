/* SPDX-License-Identifier: MIT; exact one-second accounting, no real clocks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbhost_work.h"
#include "usblog.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t now,status;
uint32_t usb_wait_now(void) {return now;}
bool usb_wait_failed(void) {return false;}
void log_line(const char *s) {CHECK(s&&*s);status++;}
int main(void) {
 usblog_clear();usbhost_stats_open(1,0x81,8000);usbhost_stats_open(2,0x81,10000);usbhost_stats_open(5,0x81,8000);
 for(unsigned d=1;d<=2;d++)for(unsigned n=0;n<(d==1?125:100);n++) {
  usbhost_stats_submit(d,0x81);usbhost_stats_token(d,0x81,true,true,false);usbhost_stats_token(d,0x81,true,true,true);
 }
 for(unsigned n=0;n<125;n++) {usbhost_stats_submit(5,0x81);usbhost_stats_token(5,0x81,true,false,false);}
 for(unsigned n=0;n<1000;n++){usbhost_stats_pass(100);usbhost_stats_irq(10);}
 now=999999;usbhost_stats_task();CHECK(!strstr(usblog_text(NULL),"USB win_ms="));
 now=1000000;usbhost_stats_task();const char *s=usblog_text(NULL);
 CHECK(strstr(s,"USB win_ms=1000 split=450 passes=1000 task_us=100000\n"));
 CHECK(strstr(s,"USB irq_us=10000 max_irq=10 max_pass=100 over=0\n"));
 CHECK(strstr(s,"D1 ep81 submit=125 poll=125 ss=125 cs=125 us=8000\n"));
 CHECK(strstr(s,"D2 ep81 submit=100 poll=100 ss=100 cs=100 us=10000\n"));
 CHECK(strstr(s,"D5 ep81 submit=125 poll=125 ss=0 cs=0 us=8000\n"));
 CHECK(status==0);
 now=2000000;usbhost_stats_task();s=usblog_text(NULL);CHECK(strstr(s,"split=0 passes=0 task_us=0"));
 CHECK(strstr(s,"submit=0 poll=0 ss=0 cs=0")); /* window deltas, no double counting */
 usbhost_stats_pass(1001);usbhost_stats_irq(250);usbhost_stats_close(2,0x81);
 now=5000000;usbhost_stats_task();s=usblog_text(NULL);
 CHECK(strstr(s,"win_ms=3000")&&strstr(s,"max_irq=250 max_pass=1001 over=1"));
 CHECK(strstr(s,"us=10000 closed"));CHECK(status==5);
 for(unsigned i=0;i<200;i++)usblog_add("enumeration filler .........................................................");
 uint32_t bytes;s=usblog_text(&bytes);CHECK(bytes<=USBLOG_MAX&&strstr(s,"log full")&&strstr(s,"over=1"));
 usblog_add_critical("DWC2 timeout critical");s=usblog_text(NULL);CHECK(strstr(s,"DWC2 timeout critical")&&strstr(s,"over=1"));
 for(unsigned window=0;window<64;window++) {
  for(unsigned n=0;n<window;n++)usbhost_stats_submit(1,0x81);
  now+=1000000;usbhost_stats_task();
 }
 s=usblog_text(&bytes);CHECK(bytes<=USBLOG_MAX&&strstr(s,"DWC2 timeout critical"));
 CHECK(strstr(s,"D1 ep81 submit=63 poll=0")&&!strstr(s,"submit=125 poll=125"));
 CHECK(strstr(s,"USB win_ms=")&&strstr(s,"over=1"));
 usblog_clear();CHECK(!*usblog_text(NULL));
 CHECK(usbhost_interval_us(false,1)==8000&&usbhost_interval_us(false,8)==8000&&usbhost_interval_us(false,10)==10000);
 CHECK(usbhost_interval_us(true,1)==8000&&usbhost_interval_us(true,7)==8000&&usbhost_interval_us(true,8)==16000);
 puts("USB accounting: exact one-second submit/poll/SS/CS, task/IRQ times, overruns, five-second status and full-log retention OK");
 return 0;
}
