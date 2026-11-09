/* SPDX-License-Identifier: MIT. HDMI boot overlay panel and recovery console. Pure: draws into
 * a pixel buffer through fb_info (pitch, 32 bpp, native pixel order), so the host renders the
 * same screens to PNG. The art is baked by scripts/make_boot_art.py from art/boot/. */
#include "kernel.h"
#include "fb_art.h"
#include "logo.h"
#include "font.h"
#define COLS 60u
#define ROWS 12u
#define KEEP 32u
static uint32_t *at(const struct fb_info *f,uint8_t *p,uint32_t x,uint32_t y) {return (uint32_t *)(void *)(p+(size_t)y*f->pitch+(size_t)x*4u);}
static void rect(const struct fb_info *f,uint8_t *p,uint32_t x0,uint32_t y0,uint32_t w,uint32_t h,uint32_t rgb) {
 uint32_t c=fb_native(f,rgb);
 for(uint32_t y=y0;y<y0+h;y++) {volatile uint32_t *r=at(f,p,x0,y);for(uint32_t x=0;x<w;x++) r[x]=c;}
}
static void blit(const struct fb_info *f,uint8_t *p,const struct art_runs *a,uint32_t x0,uint32_t y0) {
 uint32_t pal[16],x=0,y=0;
 for(unsigned i=0;i<a->colors;i++) pal[i]=fb_native(f,a->palette[i]);
 for(unsigned i=0;i<a->bytes;i++) {
  for(unsigned n=(a->runs[i]&15u)+1u;n;n--) {*at(f,p,x0+x,y0+y)=pal[a->runs[i]>>4];if(++x==a->w) {x=0;y++;}}
 }
}
static bool fits(const struct fb_info *f,uint32_t w,uint32_t h) {return f->depth==32&&f->width==w&&f->height==h;}
void fb_overlay(const struct fb_info *f,uint8_t *pixels) {
 if(fits(f,960,540)) blit(f,pixels,&art_panel,298,312);
 else if(fits(f,640,480)) blit(f,pixels,&art_panel,138,272);
}
/* The log: KEEP rows of COLS characters, newest last in time (ring). */
static char rows[KEEP][COLS];
static uint32_t used,gen;
static void new_row(void) {memset(rows[used%KEEP],' ',COLS);used++;gen++;}
void console_log(const char *s) {
 new_row();unsigned col=0;
 for(;*s;s++) {
  unsigned char c=(unsigned char)*s;
  if(c<' '||c>'~') continue;
  if(col==COLS) {new_row();col=0;}
  rows[(used-1)%KEEP][col++]=(char)c;
 }
}
uint32_t console_gen(void) {return gen;}
static void glyph(const struct fb_info *f,uint8_t *p,uint32_t x,uint32_t y,char ch) {
 const uint8_t *g=font_glyph((uint8_t)ch);
 for(unsigned r=0;r<7;r++) for(unsigned c=0;c<5;c++) {
  bool on=!g?(ch!=' '&&(r==0||r==6||c==0||c==4)):(g[r]>>(4-c))&1;
  if(on) rect(f,p,x+c*2,y+r*2,2,2,0xffffff);
 }
}
void fb_console_log(const struct fb_info *f,uint8_t *pixels) {
 if(!fits(f,960,540)) return;
 rect(f,pixels,106,137,748,316,0);
 uint32_t first=used>ROWS?used-ROWS:0;
 for(uint32_t i=first;i<used;i++) for(unsigned c=0;c<COLS;c++) glyph(f,pixels,120+c*12,151+(i-first)*24,rows[i%KEEP][c]);
}
bool fb_console(const struct fb_info *f,uint8_t *pixels) {
 if(!fits(f,960,540)) return false;
 rect(f,pixels,0,0,960,540,0x101420);
 rect(f,pixels,24,24,912,2,0x7888a0);rect(f,pixels,24,514,912,2,0x7888a0);
 rect(f,pixels,104,135,752,320,0x7888a0); /* frame, 2 px */
 blit(f,pixels,&art_header,96,44);
 fb_console_log(f,pixels);
 return true;
}
