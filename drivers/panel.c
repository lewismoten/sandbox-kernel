/* SPDX-License-Identifier: MIT. Original async SPI/renderer; SSD1680 V4
 * register sequence adapted from Waveshare team, permission in licenses/. */
#include "kernel.h"
#define SPI (PERIPH+0x204000)
static uint8_t frame[4000],desired[4000],shown[4000];
static unsigned phase,pos;
static uint32_t since,delay;
static bool full=true,disabled;
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:./ ";
/* Original 3x5 glyphs, rows encoded as octal digits; no imported font. */
static const uint16_t glyphs[]={075757,065656,074447,065556,074647,074644,074557,055755,072227,011156,055655,044447,057555,075555,075557,075644,075573,075655,074217,072222,055557,055552,055575,055255,055222,071247,075557,026227,071747,071717,055711,074717,074757,071111,075757,075717,000700,002020,000024,001240,000020,000000};
static void text(unsigned x,unsigned y,const char *s) {
 while(*s&&x<244) {
  unsigned ch=(uint8_t)*s++; if(ch==0xe2 && (uint8_t)s[0]==0x80 && (uint8_t)s[1]==0xa6) {
   s+=2; for(unsigned dot=0;dot<3;dot++) {unsigned px=y+8,py=249-(x+dot*2);if(px<122&&py<250)desired[py*16+px/8]&=(uint8_t)~(0x80>>(px%8));} x+=8;continue;
  }
  if(ch>='a'&&ch<='z') ch-=32;
  unsigned index=39; for(unsigned i=0;i<sizeof(alphabet)-1;i++) if(ch==(unsigned)alphabet[i]) {index=i;break;}
  uint16_t bits=glyphs[index];
  for(unsigned row=0;row<5;row++) for(unsigned col=0;col<3;col++) if(bits&(1u<<((4-row)*3+2-col)))
   for(unsigned dy=0;dy<2;dy++) for(unsigned dx=0;dx<2;dx++) {
    unsigned lx=x+col*2+dx,ly=y+row*2+dy;
    if(lx<250&&ly<122) {unsigned px=ly,py=249-lx;desired[py*16+px/8]&=(uint8_t)~(0x80>>(px%8));}
   }
  x+=8;
 }
}
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
 memset(desired,255,sizeof(desired)); text(16,48,"Starting\xe2\x80\xa6");memcpy(frame,desired,4000);
 gpio_set(17,true);wait_phase(1,20000);
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
  if(gpio_get(24)) {if(expired(since,5000000)) {disabled=true;error("Panel BUSY timeout");}return;}
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
 memset(desired,255,4000);text(8,6,"sandbox-kernel");text(8,26,BUILD_ID);
 char up[32];uptime(up,seconds);text(8,46,"Up:");text(40,46,up);text(8,66,"USB:");text(48,66,state);
 text(8,86,"Error:");text(8,102,last_error);
 if(!memcmp(desired,shown,4000))return;
 memcpy(frame,desired,4000);gpio_set(17,false);wait_phase(12,1000);
}
