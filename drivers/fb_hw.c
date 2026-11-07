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
