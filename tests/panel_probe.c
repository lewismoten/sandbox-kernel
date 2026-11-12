/* SPDX-License-Identifier: MIT. Real panel driver over simulated SPI and GPIO.
 * Timer and error reporting come from the shared syscall harness. */
#include "host_exports.h"
#include "kernel.h"
#include "clock.h"
static uint32_t spi[4];
static bool dc=true;
static unsigned fault,command,mode,full_count,partial_count;
static uint64_t boot_trigger,last_full_trigger,last_trigger,wake_until;
static bool sleeping,reset_pin=true;
static unsigned busy_commands,resets_after_sleep;
/* A model of the controller's RAM and of the SPI command stream, so the tests can check what reached 0x24/0x26. */
static uint8_t ram24[4000],ram26[4000];
static unsigned mwx0,mwx1,mwy0,mwy1,cx,cy,nparam,logn,ram_writes;
static uint8_t params[8];
struct entry {uint8_t cmd,first[8];unsigned count;};
static struct entry logv[512];
static void ram_put(uint8_t b) {
 uint8_t *r=command==0x24?ram24:ram26;
 if(cy<250&&cx<16) r[cy*16+cx]=b;
 if(cx>=mwx1) {cx=mwx0;cy++;} else cx++;
 ram_writes++;
}
void advance(uint32_t us);
static volatile uint32_t *panel_reg(uint32_t address) {
 unsigned index=(address-(PERIPH+0x204000))/4;
 if(index==0&&spi[0]==0xb0) {
  if(fault==2) advance(1001);
  else {
   unsigned b=spi[1]&255;
   if(sleeping||clock_us()<wake_until) busy_commands++; /* a write while asleep, or while BUSY is high after a wake reset: the controller ignores it */
   if(!dc) {
    command=b;nparam=0;
    if(logn<512) {logv[logn].cmd=b;logv[logn].count=0;memset(logv[logn].first,0,8);logn++;}
    if(b==0x20) {
     last_trigger=clock_us();
     if(mode==0xf7) {
      if(!full_count)boot_trigger=last_trigger;
      full_count++;last_full_trigger=last_trigger;
     } else partial_count++;
    }
   } else {
    if(logn) {struct entry *e=&logv[logn-1];if(e->count<8)e->first[e->count]=(uint8_t)b;e->count++;}
    if(nparam<8)params[nparam]=(uint8_t)b;
    nparam++;
    if(command==0x44&&nparam==2){mwx0=params[0];mwx1=params[1];}
    if(command==0x45&&nparam==4){mwy0=params[0]|params[1]<<8;mwy1=params[2]|params[3]<<8;}
    if(command==0x4e&&nparam==1)cx=b;
    if(command==0x4f&&nparam==2)cy=params[0]|params[1]<<8;
    if(command==0x24||command==0x26)ram_put((uint8_t)b);
    if(command==0x22)mode=b;
    else if(command==0x10&&b==1)sleeping=true; /* deep sleep mode 1: wake needs a hardware reset, then a BUSY-high settle (model: 10 ms) */
   }
   spi[0]=0x10000;
  }
 }
 return &spi[index];
}
#undef REG
#define REG(a) (*panel_reg(a))
#define BUILD_ID "PANEL TEST"
void gpio_mode(unsigned pin,unsigned mode_) {(void)pin;(void)mode_;}
void gpio_set(unsigned pin,bool value) {
 if(pin==25)dc=value;
 if(pin==17) {if(value&&!reset_pin&&sleeping) {sleeping=false;wake_until=clock_us()+10000;resets_after_sleep++;}reset_pin=value;}
}
bool gpio_get(unsigned pin) {(void)pin;return fault==1||sleeping||clock_us()<wake_until;} /* BUSY is high in deep sleep and after the wake reset */
#include "../drivers/panel.c"
void panel_probe_fault(unsigned kind) {fault=kind;}
bool panel_probe_disabled(void) {return disabled;}
unsigned panel_probe_full_count(void) {return full_count;}
unsigned panel_probe_partial_count(void) {return partial_count;}
uint64_t panel_probe_boot_trigger(void) {return boot_trigger;}
uint64_t panel_probe_last_full_trigger(void) {return last_full_trigger;}
uint64_t panel_probe_last_trigger(void) {return last_trigger;}
unsigned panel_probe_busy_commands(void) {return busy_commands;}
unsigned panel_probe_wakes(void) {return resets_after_sleep;}
void panel_probe_log_reset(void) {logn=0;}
unsigned panel_probe_log_count(void) {return logn;}
/* One entry per command: byte 0 = command, 1..8 = first data bytes, 9..12 = data byte count (little endian). */
void panel_probe_log_entry(unsigned i,uint8_t out[13]) {
 out[0]=logv[i].cmd;memcpy(out+1,logv[i].first,8);out[9]=(uint8_t)logv[i].count;out[10]=(uint8_t)(logv[i].count>>8);out[11]=(uint8_t)(logv[i].count>>16);out[12]=0;
}
void panel_probe_ram(uint8_t a[4000],uint8_t b[4000]) {memcpy(a,ram24,4000);memcpy(b,ram26,4000);}
