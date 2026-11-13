/* SPDX-License-Identifier: MIT; drivers/sd.c on a tiny model of the BCM2835 EMMC registers.
 * Models only what matters here: a failed block read leaves DAT inhibit raised until the host
 * resets the DAT circuit (CONTROL1 bit 26). Host only; no real card timing. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel.h"
#undef REG
static uint32_t cmd13_bad;static uint32_t regs[0x100/4];static uint32_t now,fifo_left,dat_stuck,fail_reads,cmd12s,full_inits,busy,busy_len,wr_left,wedged,cmd13_lies;
static volatile uint32_t *sim(uintptr_t a) {
 uintptr_t o=a-(PERIPH+0x300000);
 if(regs[0x0c/4]) {
  uint32_t c=regs[0x0c/4]>>24;regs[0x0c/4]=0;regs[0x30/4]=1;regs[0x10/4]=0;
  if(c==13) { if(busy&&cmd13_lies) regs[0x10/4]=(4u<<9)|0x100; else if(busy) {busy--;regs[0x10/4]=7u<<9;} else regs[0x10/4]=cmd13_bad?(cmd13_bad--,0):(4u<<9)|0x100; }
  if(c==12) cmd12s++;
  if(c==24) {
   if(busy) {dat_stuck=1;wedged=1;} /* a command while the card programs: no data phase ever comes */
   else {regs[0x30/4]|=0x10;wr_left=128;}
  }
  if(c==17) {
   if(busy) {dat_stuck=1;wedged=1;}
   else if(fail_reads) {fail_reads--;dat_stuck=1;} /* no data-ready ever: the transfer times out */
   else {regs[0x30/4]|=0x20;fifo_left=128;}
  }
 }
 if(regs[0x2c/4]&(1u<<26)) {regs[0x2c/4]&=~(1u<<26);dat_stuck=0;}
 if(o==0x24&&busy) busy--; /* the card programs while the host polls */
 regs[0x24/4]=(dat_stuck?2:0)|(busy?0:1u<<20); /* bit 20: DAT0 level, low while busy */
if(o==0x30&&wr_left==128) regs[0x30/4]|=0x10;
if(o==0x30&&fifo_left==128) regs[0x30/4]|=0x20; /* write-1-to-clear is not modelled: ready stays up until read out */
 if(o==0x20&&wr_left&&!--wr_left) {regs[0x30/4]|=2;busy=busy_len;}
 if(o==0x20&&fifo_left&&!--fifo_left) regs[0x30/4]|=2;
 return &regs[o/4];
}
#define REG(a) (*sim(a))
#include "../drivers/sd.c"
uint32_t micros(void) {return now+=500;}
void service_io(void) {}
void delay_us(uint32_t us) {now+=us;}
bool property(uint32_t t,uint32_t *v,unsigned w) {(void)t;(void)v;(void)w;full_inits++;return false;}
void gpio_mode(unsigned p,unsigned m) {(void)p;(void)m;}
uint32_t crc32(const void *p,size_t n) {(void)p;(void)n;return 0;}
void sim_setup(uint32_t sectors) {capacity=sectors;rca=0x10000;fail_reads=0;dat_stuck=0;busy=0;busy_len=0;wr_left=0;wedged=0;cmd13_lies=0;full_inits=0;cmd12s=0;regs[0x2c/4]=4;}
void sim_fail_reads(uint32_t n) {fail_reads=n;}
uint32_t sim_cmd12s(void) {return cmd12s;}
uint32_t sim_full_inits(void) {return full_inits;}
void sim_cmd13_bad(uint32_t n) {cmd13_bad=n;}
void sim_busy_after_write(uint32_t polls) {busy_len=polls;}
uint32_t sim_wedged(void) {return wedged;}
void sim_cmd13_lies(uint32_t on) {cmd13_lies=on;} /* CMD13 says tran+ready while DAT0 is still low */
