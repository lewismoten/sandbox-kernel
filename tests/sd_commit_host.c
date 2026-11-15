/* SPDX-License-Identifier: MIT; deterministic controller/card wait simulation.
 * Runs the REAL SD transfer/command polling code, no MMIO or hardware access. */
#include "kernel.h"
#include "cache_commit.h"
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"failed: %s at %d\n",#x,__LINE__);return 1;}} while(0)
#include <stdio.h>
static uint32_t regs[64],time_us,command_seen,data_reads;
static uint32_t command_started,program_started,program_us=50000;
static bool data_phase,stuck;
static uint32_t *sd_reg(unsigned offset) {
 time_us+=1;
 unsigned idx=regs[0x0c/4]>>24;
 if(idx&&regs[0x0c/4]!=command_seen) {
  command_seen=regs[0x0c/4];command_started=time_us;
  if(idx==24||idx==17) {data_phase=true;data_reads=0;program_started=time_us;}
 }
 if(offset==0x30) {
  /* Emulate W1C INT_STATUS plus delayed CMD_DONE/ready/DATA_DONE. */
  regs[0x30/4]=0;
  if(time_us-command_started>=3000) {
   regs[0x30/4]|=1;
   if(data_phase) regs[0x30/4]|=(idx==24?0x10:0x20);
   if(data_reads>=128) regs[0x30/4]|=2;
  }
 }
 if(offset==0x20)data_reads++;
 if(offset==0x10) {
  regs[0x10/4]=(!stuck&&time_us-program_started>=program_us)?0x900:0xe00;
  /* Repeated CMD13 has identical command bits: observe each write via the
     argument register on next command() call, below. */
 }
 if(offset==0x08) {command_seen=0;data_phase=false;}
 return &regs[offset/4];
}
#define SD_REG(o) (*sd_reg(o))
#include "../drivers/sd.c"
const char *fatput_fail="none";
uint32_t micros(void) {return time_us;}
void service_io(void) {time_us+=100;} /* synchronous baseline */
/* The worker's checkpoints need a ticking clock even while no service_io runs. */
/* sd_reg called in every busy poll; advances virtual hardware time. */
int fatcache_put(const char *name,const uint8_t *data,uint32_t bytes) {
 (void)name;(void)bytes;return sd_write(10,data)?0:-5;
}
int fatsave_put(const char *game,const char *name,const uint8_t *data,uint32_t bytes) {
 (void)game;return fatcache_put(name,data,bytes);
}
/* Unused init dependencies (no board init is invoked). */
bool property(uint32_t t,uint32_t *p,unsigned n) {(void)t;(void)p;(void)n;return false;}
void gpio_mode(unsigned p,unsigned m) {(void)p;(void)m;}
void delay_us(uint32_t us) {time_us+=us;}
uint32_t crc32(const void *p,size_t n) {(void)p;(void)n;return 0;}
int main(void) {
 static const char game[9]="",name[13]="X.IDX";static uint8_t bytes[512];
 capacity=100;rca=1;
 uint32_t begin=time_us;
 CHECK(!fatcache_put(name,bytes,sizeof bytes));
 printf("SD real polling: synchronous slow-sector call %u us\n",time_us-begin);
 for(unsigned timeout=0;timeout<2;timeout++) {
  regs[0x0c/4]=0;command_seen=0;data_reads=0;data_phase=false;
  stuck=timeout!=0;
  CHECK(!cache_commit_start(game,name,bytes,sizeof bytes));
  uint32_t max=0,passes=0;
  while(cache_commit_pending()) {
   uint32_t begin=time_us;
   cache_commit_task();
   uint32_t delta=time_us-begin;if(delta>max)max=delta;
   CHECK(delta<=1250);
   CHECK(++passes<50000);
  }
  CHECK(cache_commit_state()==(timeout?3u:2u));
  CHECK(cache_commit_error()==(timeout?-5:0));
  CHECK(max>=1000);
  printf("SD real polling: %s, max slice %u us, %u passes\n",timeout?"timeout":"complete",max,passes);
 }
 return 0;
}
