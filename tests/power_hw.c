/* SPDX-License-Identifier: MIT; firmware responses are mocked, no MMIO. */
#include "kernel.h"
#include "power.h"
#include <assert.h>
#include <stdio.h>
static uint64_t now;
static uint32_t rate=1000000000u,blank;
static unsigned sets,blanks,standbys,wakes;
static bool fail,fail_min;
uint32_t input_activity,fb_mode_generation;
uint64_t clock_us(void) {return now;}
bool property(uint32_t tag,uint32_t *v,unsigned n) {
 if(fail) return false;
 switch(tag) {
 case 0x30002: assert(n==2&&v[0]==3);v[1]=rate;break;
 case 0x30007: assert(n==2&&v[0]==3);if(fail_min)return false;v[1]=700000000u;break;
 case 0x38002: assert(n==3&&v[0]==3&&v[2]==0);rate=v[1];sets++;break;
 case 0x40002: assert(n==1);blank=v[0];blanks++;break;
 default: assert(0);
 }
 return true;
}
static uint32_t async_tag,async_values[3];
static unsigned async_words;
static bool async_busy,late,late_reported;
bool property_async_start(uint32_t tag,const uint32_t *v,unsigned n) {
 assert(!async_busy);async_busy=true;async_tag=tag;async_words=n;
 for(unsigned i=0;i<n;i++)async_values[i]=v[i];
 return true;
}
int property_async_poll(uint32_t *v,unsigned n) {
 assert(async_busy);
 if(late) {if(!late_reported){late_reported=true;return -110;}return 0;}
 async_busy=false;
 if(!property(async_tag,async_values,async_words)) return -5;
 for(unsigned i=0;i<n;i++)v[i]=async_values[i];
 return 1;
}
static void pump(void) {for(unsigned i=0;i<4;i++)power_hw_task();}
void log_line(const char *s) {assert(s&&s[0]);}
void cec_hw_power(bool standby) {if(standby)standbys++;else wakes++;}
int main(void) {
 power_hw_init();
 now=59999999;pump();assert(!sets&&!blanks);
 now=60000000;pump();assert(sets==1&&rate==700000000u);
 now=600000000;pump();assert(blank==1&&standbys==1);
 /* Mode allocation may unblank without input: reassert policy. */
 blank=0;fb_mode_generation++;pump();assert(blank==1);
 input_activity++;pump();assert(rate==1000000000u&&!blank&&wakes==1);
 uint64_t token;assert(!power_acquire(3,&token));
 now+=7200000000ull;pump();assert(rate==1000000000u&&!blank);
 assert(!power_release(token));pump();assert(rate==700000000u&&blank);
 /* CEC input calls the clock-based policy directly, without input reader activity. */
 power_input(now);pump();assert(rate==1000000000u&&!blank);
 now+=600000000;fail=true;unsigned old=sets;pump();
 fail=false;now++;pump();assert(sets==old); /* retry bounded */
 now+=5000000;pump();assert(sets==old+1&&blank);
 /* Wake is immediate even during backoff from a failed blank operation. */
 fail=true;power_input(now);pump();fail=false;
 uint64_t hold;assert(!power_acquire(3,&hold));
 now+=5000001;pump();assert(!blank&&rate==1000000000u);
 power_reset(now);pump();assert(!blank);
 /* Firmware can hold a request across many passes; no foreground wait.
    Input arriving during that request must eventually restore the fast clock. */
 now+=60000000;late=true;pump();assert(async_busy&&rate==1000000000u);
 power_input(now);pump();assert(async_busy);
 late=false;pump();assert(!async_busy&&rate==1000000000u);
 fail_min=true;power_hw_init();now+=60000000;pump();assert(rate==700000000u);
 puts("PASS power mailbox transitions, holds, input, failures and fallback");
}
