/* SPDX-License-Identifier: MIT. Original async SPI/renderer; SSD1680 V4
 * register sequence adapted from Waveshare team, permission in licenses/. */
#include "kernel.h"
#include "render.h"
#include "eink.h"
#include "clock.h"
#define SPI (PERIPH+0x204000)
static uint8_t frame[4000],desired[4000],shown[4000];
static unsigned phase,pos;
static uint32_t since,delay;
static bool full=true,disabled;
static bool byte(uint8_t b) {
 REG(SPI)=0xb0; REG(SPI+4)=b; uint32_t t=micros();
 while(!(REG(SPI)&0x10000)) if(expired(t,1000)) {disabled=true;error("SPI timeout");REG(SPI)=0;return false;}
 (void)REG(SPI+4); REG(SPI)=0; return true;
}
static void cmd(uint8_t c) { gpio_set(25,false); byte(c); gpio_set(25,true); }
static void datum(uint8_t b) {byte(b);}
static void window(void) {
 cmd(0x44);datum(0);datum(15);cmd(0x45);datum(0);datum(0);datum(249);datum(0);
 cmd(0x4e);datum(0);cmd(0x4f);datum(0);datum(0);
}
static void setup(void) {cmd(1);datum(249);datum(0);datum(0);cmd(0x11);datum(3);window();}
static void wait_phase(unsigned next,uint32_t us) {phase=next;since=micros();delay=us;}
void panel_start(void) {
 gpio_mode(8,4);gpio_mode(10,4);gpio_mode(11,4);gpio_mode(25,1);gpio_mode(17,1);gpio_mode(24,0);
 REG(SPI+8)=64; /* 250MHz core /64 = 3.90625 MHz; fixed in config.txt */
 render_starting(desired);memcpy(frame,desired,4000);
 gpio_set(17,true);wait_phase(1,20000);
 eink_set_available(true);eink_note_refresh(true,clock_us()); /* the boot refresh is a full one: count it in the cadence ledger */
}
bool panel_boot_done(void) {return disabled||!full;}
void panel_task(const char *state,uint32_t seconds) {
 if(disabled)return;
 if(phase && phase<=3 && !expired(since,delay))return;
 switch(phase) {
 case 1:gpio_set(17,false);wait_phase(2,2000);return;
 case 2:gpio_set(17,true);wait_phase(3,20000);return;
 case 3:wait_phase(4,5000000);return;
 case 4:case 5:case 6:case 9:
  if(phase!=4&&!expired(since,10000))return;
  if(gpio_get(24)) {if(expired(since,5000000)) {disabled=true;eink_set_available(false);error("Panel BUSY timeout");}return;}
  if(phase==4) {cmd(0x12);wait_phase(5,0);return;}
  if(phase==5) {setup();cmd(0x3c);datum(5);cmd(0x21);datum(0);datum(0x80);cmd(0x18);datum(0x80);wait_phase(6,0);return;}
  if(phase==6) {window();cmd(0x24);pos=0;phase=7;return;}
  /* Refresh completed; synchronize previous plane before accepting new image. */
  window();cmd(0x26);pos=0;phase=10;return;
 case 7:case 8:case 10: {
  unsigned stop=pos+128;if(stop>4000)stop=4000;while(pos<stop&&!disabled)datum(frame[pos++]);if(pos<4000)return;
  if(phase==7&&full) {window();cmd(0x26);pos=0;phase=8;return;}
  if(phase==10) {memcpy(shown,frame,4000);full=false;wait_phase(11,10000);return;}
  cmd(0x22);datum(full?0xf7:0xff);cmd(0x20);wait_phase(9,0);return;
 }
 case 11:if(!expired(since,delay))return;phase=0;return;
 case 12:if(!expired(since,1000))return;gpio_set(17,true);cmd(0x3c);datum(0x80);setup();cmd(0x24);pos=0;phase=7;return;
 default:break;
 }
 if(phase)return;
 eink_compose(desired,BUILD_ID,state,last_error,seconds); /* the OS page or title, or the kernel screen (docs/eink.md) */
 {uint64_t now=clock_us();if(!memcmp(desired,shown,4000)&&!(eink_maintenance_due(now)&&eink_full_due(now)))return;full=eink_full_due(now);eink_note_refresh(full,now);} /* ghost-clearing full refresh: VERIFY on the board */
 memcpy(frame,desired,4000);gpio_set(17,false);wait_phase(12,1000);
}
