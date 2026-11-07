/* SPDX-License-Identifier: MIT; HDMI framebuffer bring-up for the runtime:
 * one multi-tag mailbox message (bounded timeout), validation in core/fb.c,
 * an uncached execute-never mapping, then the test pattern. Logs one line. */
#include "kernel.h"
#include "fb.h"
const char *fb_fail="none";
struct fb_info fb0;
uint8_t *fb_pixels;
static void append(char **p,const char *s) {while(*s)*(*p)++=*s++; **p=0;}
static void append_n(char **p,uint32_t n) {char t[12];number(t,n);append(p,t);}
static void append_hex(char **p,uint32_t n) {append(p,"0x");for(int i=28;i>=0;i-=4)*(*p)++="0123456789abcdef"[(n>>i)&15];**p=0;}
bool fb_start(void) {
 char line[64],*p=line;
 uint32_t d[2]={0,0};
 if(property(0x40003,d,2)) {append(&p,"FB display ");append_n(&p,d[0]);append(&p,"x");append_n(&p,d[1]);}
 else {append(&p,"FB display unknown ");append(&p,property_fail);}
 boot_note(line);
 uint32_t m[FB_MESSAGE_WORDS]; const char *why=NULL;
 for(unsigned attempt=0;attempt<2&&(!attempt||why);attempt++) {
  fb_request(m,FB_WIDTH,FB_HEIGHT,FB_DEPTH);
  why=property_message(m,FB_MESSAGE_WORDS,500000)?fb_parse(m,FB_DEPTH,&fb0):"FB MBOX";
 }
 if(!why&&!mmu_map_uncached(fb0.phys,fb0.bytes)) why="FB MAP";
 p=line;
 if(why) {fb_fail=why;append(&p,why);if(!strcmp(why,"FB MBOX")){append(&p," ");append(&p,property_fail);}boot_note(line);return false;}
 fb_pixels=(uint8_t *)(uintptr_t)fb0.phys;
 fb_pattern(&fb0,fb_pixels);
 append(&p,"FB ok ");append_n(&p,fb0.width);append(&p,"x");append_n(&p,fb0.height);append(&p," ");append_n(&p,fb0.depth);append(&p," ");append_n(&p,fb0.pitch);
 boot_note(line);
 p=line;append(&p,"FB bus ");append_hex(&p,fb0.bus);append(&p," bytes ");append_n(&p,fb0.bytes);append(&p," order ");append_n(&p,fb0.pixel_order);
 boot_note(line);
 return true;
}
/* ABI 1.2 mode sets and palettes, between a program's slices (no user code
   runs during the mailbox call). The old buffer is left to the firmware. */
int fb_set_mode(uint32_t w,uint32_t h,uint32_t d) {
 uint32_t t=micros(),m[FB_MESSAGE_WORDS]; struct fb_info f;
 fb_request(m,w,h,d);
 if(!property_message(m,FB_MESSAGE_WORDS,500000)) {fb_fail="FB MBOX";return -5;}
 const char *why=fb_parse(m,d,&f);
 if(why) {fb_fail=why;return -5;}
 if(!mmu_map_uncached(f.phys,f.bytes)) {fb_fail="FB MAP";return -5;}
 fb0=f;fb_pixels=(uint8_t *)(uintptr_t)f.phys;
 for(uint32_t i=0;i<f.bytes/4u;i++) ((volatile uint32_t *)(void *)fb_pixels)[i]=0;
 fb_mode_us=micros()-t;
 char line[64],*p=line;append(&p,"FB mode ");append_n(&p,w);append(&p,"x");append_n(&p,h);append(&p,"x");append_n(&p,d);
 append(&p," in ");append_n(&p,fb_mode_us);append(&p," us");log_line(line);
 return 0;
}
int fb_set_palette(uint32_t first,uint32_t count,const uint32_t *rgb) {
 static uint32_t m[FB_PALETTE_WORDS] __attribute__((aligned(16)));
 uint32_t words=fb_palette_request(m,first,count,rgb);
 return property_message(m,words,500000)&&m[1]==0x80000000u&&!m[5]?0:-5;
}
