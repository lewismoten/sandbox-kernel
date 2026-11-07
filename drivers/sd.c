/* SPDX-License-Identifier: MIT; BCM2835 EMMC chapter 5, SD physical commands.
 * PIO single-block only; no dependency on vendor/Circle/Linux SD code. */
#include "kernel.h"
#define SD (PERIPH+0x300000)
#define R(o) REG(SD+(o))
static bool high_capacity;
static uint32_t capacity,rca;
static bool wait_bits(unsigned reg,uint32_t mask,uint32_t value,uint32_t timeout) {
 uint32_t t=micros(); while((R(reg)&mask)!=value) { service_io(); if(expired(t,timeout)) return false; } return true;
}
static bool command(unsigned idx,uint32_t arg,unsigned flags) {
 if(!wait_bits(0x24,1,0,100000)) return false;
 R(0x30)=~0u; R(0x08)=arg; barrier(); R(0x0c)=(idx<<24)|flags;
 uint32_t t=micros(); while(!(R(0x30)&0x8001)) { service_io(); if(expired(t,100000)) return false; }
 uint32_t s=R(0x30); R(0x30)=s&0xffff0001u;
 return !(s&0xffff8000u);
}
static bool clock_set(uint32_t hz,uint32_t base) {
 if(!wait_bits(0x24,3,0,100000)) return false;
 R(0x2c)&=~4u;
 uint32_t div=(base+hz*2-1)/(hz*2); if(div>1023) return false;
 R(0x2c)=(R(0x2c)&~0xffe0u)|((div&255)<<8)|((div&768)>>2)|1;
 if(!wait_bits(0x2c,2,2,100000)) return false;
 R(0x2c)|=4; delay_us(2000); return true;
}
const char *sd_fail="none";
#define FAIL(code) do { sd_fail=(code); return false; } while(0)
/* One attempt from whatever state the previous owner (the selector) left the
 * controller and card in: card selected, 25 MHz, SD clock running. */
static bool sd_init_once(void) {
 capacity=0; rca=0;
 uint32_t power[2]={0,3}; if(!property(0x28001,power,2)) FAIL("SD POWER");
 for(unsigned i=48;i<=53;i++) gpio_mode(i,7); /* ALT3 SD1 */
 REG(PERIPH+0x200094)=2; delay_us(5); REG(PERIPH+0x20009c)=0x3f0000; delay_us(5); REG(PERIPH+0x200094)=0; REG(PERIPH+0x20009c)=0;
 /* Stop the clock first so the card sees a clean restart, then reset the host. */
 R(0x2c)&=~4u; delay_us(100);
 R(0x30)=~0u; R(0x38)=0;
 R(0x2c)=1u<<24; if(!wait_bits(0x2c,7u<<24,0,100000)) FAIL("SD RESET");
 R(0x2c)=0xe0000; R(0x38)=0; R(0x34)=~0u; R(0x30)=~0u;
 uint32_t clk[2]={1,0}; if(!property(0x30002,clk,2)||!clk[1]) FAIL("SD CLKRATE");
 if(!clock_set(400000,clk[1])) FAIL("SD CLOCK");
 delay_us(10000); /* at least 74 clocks before CMD0 */
 /* CMD0 returns a card left in transfer state to idle. Send it twice. */
 if(!command(0,0,0)) FAIL("SD CMD0");
 delay_us(2000);
 if(!command(0,0,0)) FAIL("SD CMD0");
 delay_us(2000);
 if(!command(8,0x1aa,0x1a0000)) FAIL("SD CMD8");
 if((R(0x10)&0xfff)!=0x1aa) FAIL("SD CMD8 ECHO"); /* SD v2 only */
 uint32_t t=micros(),ocr;
 do {
  if(!command(55,0,0x1a0000)) FAIL("SD CMD55");
  if(!command(41,0x40ff8000,0x20000)) FAIL("SD ACMD41");
  ocr=R(0x10); if(expired(t,1500000)) FAIL("SD ACMD41 BUSY"); delay_us(1000);
 } while(!(ocr&0x80000000u));
 high_capacity=(ocr&(1u<<30))!=0; if(!high_capacity) FAIL("SD NOT SDHC");
 if(!command(2,0,0x90000)) FAIL("SD CMD2");
 if(!command(3,0,0x1a0000)) FAIL("SD CMD3");
 rca=R(0x10)&0xffff0000u;
 if(!rca) FAIL("SD RCA");
 if(!command(9,rca,0x90000)) FAIL("SD CMD9");
 /* SDHCI strips CRC and shifts R2 by 8. CSD v2 bits 69:48. */
 uint32_t csd1=R(0x14),csd3=R(0x1c);
 if((csd3>>22)!=1) FAIL("SD CSD VER");
 uint32_t csize=(csd1>>8)&0x3fffff;
 if(csize>=0x3fffff) FAIL("SD CSD SIZE");
 capacity=(csize+1)*1024;
 if(!command(7,rca,0x1b0000)) FAIL("SD CMD7");
 if(!wait_bits(0x24,2,0,100000)) FAIL("SD CMD7 BUSY");
 if(!clock_set(25000000,clk[1])) FAIL("SD CLOCK25");
 return true; /* 1-bit bus, 512-byte blocks */
}
bool sd_init(void) {
 sd_fail="none";
 /* A card the previous stage left selected or mid-state can need a second try. */
 for(unsigned attempt=0;attempt<3;attempt++) {
  if(sd_init_once()) return true;
  delay_us(20000);
 }
 return false; /* sd_fail names the step of the last attempt */
}
uint32_t sd_sectors(void) { return capacity; }
static bool transfer(uint32_t lba,void *p,bool write) {
 if(lba>=capacity) return false;
 if(!wait_bits(0x24,3,0,100000)) return false;
 R(0x04)=(1<<16)|512;
 if(!command(write?24:17,lba,0x3a0000|(write?0:0x10))) return false;
 uint32_t t=micros(),ready=write?0x10:0x20;
 while(!(R(0x30)&(ready|0xffff8000u))) { service_io(); if(expired(t,250000)) return false; }
 if(R(0x30)&0xffff8000u) return false;
 R(0x30)=ready;
 uint8_t *b=p;
 for(unsigned i=0;i<128;i++) { uint32_t w; if(write) { memcpy(&w,b+i*4,4); R(0x20)=w; } else { w=R(0x20); memcpy(b+i*4,&w,4); } }
 t=micros(); while(!(R(0x30)&0xffff8002u)) { service_io(); if(expired(t,500000)) return false; }
 if(R(0x30)&0xffff8000u) return false; R(0x30)=2;
 /* CMD13 polls card programming state: host DATA_DONE alone is insufficient. */
 if(write) {
  t=micros(); do { if(!command(13,rca,0x1a0000)) return false; uint32_t status=R(0x10); if(status&0xfdffe008u) return false; if((status&0x1f00)==0x900) return true; service_io(); } while(!expired(t,1000000)); return false;
 }
 return true;
}
bool sd_read(uint32_t lba,void *p) { return transfer(lba,p,false); }
bool sd_write(uint32_t lba,const void *p) { return transfer(lba,(void *)p,true); }
