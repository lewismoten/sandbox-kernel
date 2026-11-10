/* SPDX-License-Identifier: MIT. Real panel driver over simulated SPI and GPIO.
 * Timer and error reporting come from the shared syscall harness. */
#include "host_exports.h"
#include "kernel.h"
#include "clock.h"
static uint32_t spi[4];
static bool dc=true;
static unsigned fault,command,mode,full_count,partial_count;
static uint64_t boot_trigger,last_full_trigger,last_trigger;
void advance(uint32_t us);
static volatile uint32_t *panel_reg(uint32_t address) {
 unsigned index=(address-(PERIPH+0x204000))/4;
 if(index==0&&spi[0]==0xb0) {
  if(fault==2) advance(1001);
  else {
   unsigned b=spi[1]&255;
   if(!dc) {
    command=b;
    if(b==0x20) {
     last_trigger=clock_us();
     if(mode==0xf7) {
      if(!full_count)boot_trigger=last_trigger;
      full_count++;last_full_trigger=last_trigger;
     } else partial_count++;
    }
   } else if(command==0x22)mode=b;
   spi[0]=0x10000;
  }
 }
 return &spi[index];
}
#undef REG
#define REG(a) (*panel_reg(a))
#define BUILD_ID "PANEL TEST"
void gpio_mode(unsigned pin,unsigned mode_) {(void)pin;(void)mode_;}
void gpio_set(unsigned pin,bool value) {if(pin==25)dc=value;}
bool gpio_get(unsigned pin) {(void)pin;return fault==1;}
#include "../drivers/panel.c"
void panel_probe_fault(unsigned kind) {fault=kind;}
bool panel_probe_disabled(void) {return disabled;}
unsigned panel_probe_full_count(void) {return full_count;}
unsigned panel_probe_partial_count(void) {return partial_count;}
uint64_t panel_probe_boot_trigger(void) {return boot_trigger;}
uint64_t panel_probe_last_full_trigger(void) {return last_full_trigger;}
uint64_t panel_probe_last_trigger(void) {return last_trigger;}
