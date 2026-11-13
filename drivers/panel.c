/* SPDX-License-Identifier: MIT. Original async SPI/renderer; SSD1680 V4
 * register sequence adapted from Waveshare team, permission in licenses/. */
#include "kernel.h"
#include "render.h"
#include "eink.h"
#include "clock.h"
#include "shot.h"
#include "power.h"
#ifndef PANEL_ROTATE_180
#define PANEL_ROTATE_180 1
#endif
static uint8_t sent[4000];
static bool sent_valid;
bool panel_snapshot(uint8_t out[4000]) {if(!sent_valid)return false;memcpy(out,sent,4000);return true;}
uint32_t panel_snapshot_order(void) {return PANEL_ROTATE_180;}
#define SPI (PERIPH+0x204000)
static uint8_t frame[4000],desired[4000],shown[4000];
static unsigned phase,pos,total;
static uint8_t wx0,wx1=15; static unsigned wy0,wy1=249; /* RAM window: byte columns and rows being written; a partial update narrows it to what changed */
static uint32_t since,delay;
static bool full=true,disabled,asleep,sleep_full; /* sleep_full: SLEEP is showing and a full refresh is still owed, to clear partial-refresh defects before the picture freezes */
static bool raw_asleep,drawn_valid; static uint64_t raw_since,last_draw; static uint32_t drawn_gen;
static uint32_t frozen; /* uptime when power saving began: while asleep the screen stays as it was */
static void disable(const char *reason) {disabled=true;eink_set_available(false);error(reason);}
static bool byte(uint8_t b) {
 if(disabled)return false;
 REG(SPI)=0xb0; REG(SPI+4)=b; uint32_t t=micros();
 while(!(REG(SPI)&0x10000)) if(expired(t,1000)) {disable("SPI timeout");REG(SPI)=0;return false;}
 (void)REG(SPI+4); REG(SPI)=0; return true;
}
static void cmd(uint8_t c) { gpio_set(25,false); byte(c); gpio_set(25,true); }
static void datum(uint8_t b) {byte(b);}
static void window(void) {
 cmd(0x44);datum(wx0);datum(wx1);cmd(0x45);datum(wy0&255);datum(wy0>>8);datum(wy1&255);datum(wy1>>8);
 cmd(0x4e);datum(wx0);cmd(0x4f);datum(wy0&255);datum(wy0>>8);
 total=(unsigned)(wx1-wx0+1)*(wy1-wy0+1);
}
static uint8_t window_byte(unsigned i) {unsigned w=(unsigned)(wx1-wx0+1);return frame[(wy0+i/w)*16+wx0+i%w];} /* i-th byte of the window, rows of 16 bytes */
static void whole_window(void) {wx0=0;wx1=15;wy0=0;wy1=249;}
/* The smallest window holding every byte that differs from what is on the glass. */
static void changed_window(void) {
 unsigned x0=16,x1=0,y0=250,y1=0;
 for(unsigned y=0;y<250;y++) for(unsigned x=0;x<16;x++) if(frame[y*16+x]!=shown[y*16+x]) {if(x<x0)x0=x;if(x>x1)x1=x;if(y<y0)y0=y;y1=y;}
 if(y0>y1) {whole_window();return;}
 wx0=(uint8_t)x0;wx1=(uint8_t)x1;wy0=y0;wy1=y1;
}
static void setup(void) {cmd(1);datum(249);datum(0);datum(0);cmd(0x11);datum(3);window();}
static void wait_phase(unsigned next,uint32_t us) {phase=next;since=micros();delay=us;}
void panel_start(void) {
 gpio_mode(8,4);gpio_mode(10,4);gpio_mode(11,4);gpio_mode(25,1);gpio_mode(17,1);gpio_mode(24,0);
 REG(SPI+8)=64; /* 250MHz core /64 = 3.90625 MHz; fixed in config.txt */
 render_starting(desired);memcpy(frame,desired,4000);
 gpio_set(17,true);wait_phase(1,20000);
 eink_set_available(true);
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
  if(gpio_get(24)) {if(expired(since,5000000)) {disable("Panel BUSY timeout");}return;}
  if(phase==4) {cmd(0x12);wait_phase(5,0);return;}
  if(phase==5) {setup();cmd(0x3c);datum(5);cmd(0x21);datum(0);datum(0x80);cmd(0x18);datum(0x80);wait_phase(6,0);return;}
  if(phase==6) {window();cmd(0x24);pos=0;phase=7;return;}
  /* Refresh completed; synchronize previous plane before accepting new image. */
  window();cmd(0x26);pos=0;phase=10;return;
 case 7:case 8:case 10: {
  unsigned stop=pos+128;if(stop>total)stop=total;while(pos<stop&&!disabled)datum(window_byte(pos++));if(pos<total)return;
  if(phase==7&&full) {window();cmd(0x26);pos=0;phase=8;return;}
  if(phase==10) {memcpy(shown,frame,4000);full=false;cmd(0x10);datum(1);wait_phase(11,10000);return;}
  cmd(0x22);datum(full?0xf7:0xff);cmd(0x20);
  /* Count boot and later refreshes at the successful command trigger, never at scheduling. */
  if(!disabled) {memcpy(sent,frame,4000);sent_valid=true;eink_note_refresh(full,clock_us());}
  wait_phase(9,0);return;
 }
 case 11:if(!expired(since,delay))return;phase=0;return;
 case 12:if(!expired(since,1000))return;gpio_set(17,true);wait_phase(13,20000);return; /* reset released: the controller wakes from deep sleep */
 case 13: /* BUSY stays high while it wakes: no command until it falls (after the 20 ms settle the cold boot also waits) */
  if(!expired(since,delay))return;
  if(gpio_get(24)) {if(expired(since,5000000)) disable("Panel BUSY timeout");return;}
  cmd(0x3c);datum(0x80);cmd(0x18);datum(0x80);setup();cmd(0x24);pos=0;phase=7;return;
 default:break;
 }
 if(phase)return;
 {uint64_t now=clock_us();bool raw=(power_target(now)&POWER_NO_BLANK)!=0; /* SLEEP = the screen-blank stage only (the bit that silences audio); a slowed but lit screen keeps the uptime */if(raw!=raw_asleep) {raw_asleep=raw;raw_since=now;}
  /* SLEEP is debounced: blank must hold 30 s to show it, and 10 s awake to clear it (docs/eink.md) */
  if(raw&&!asleep&&now-raw_since>=EINK_SLEEP_SHOW_US) {asleep=true;sleep_full=true;frozen=seconds;}
  else if(!raw&&asleep&&now-raw_since>=EINK_SLEEP_CLEAR_US) {asleep=false;sleep_full=false;}
  render_status_sleep(asleep);}
 eink_compose(desired,BUILD_ID,state,last_error,asleep?frozen:seconds); /* the OS page or title, or the kernel screen (docs/eink.md) */
 {uint64_t now=clock_us();bool changed=memcmp(desired,shown,4000)!=0,maint=eink_maintenance_due(now)&&eink_full_due(now),owed=sleep_full&&asleep&&eink_full_allowed(now);
  if(!changed&&!maint&&!owed)return;
  /* at most one redraw per 10 s, unless the OS or the kernel's error text asked for a new picture */
  if(drawn_valid&&eink_generation()==drawn_gen&&!maint&&!owed&&now-last_draw<EINK_REDRAW_MIN_US)return;
  drawn_valid=true;drawn_gen=eink_generation();last_draw=now;full=eink_full_due(now)||owed;if(full)sleep_full=false;}
 memcpy(frame,desired,4000);
 if(full) whole_window(); else changed_window(); /* a partial writes (and syncs 0x26 for) only the rectangle that changed */
 gpio_set(17,false);wait_phase(12,1000);
}
