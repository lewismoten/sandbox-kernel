/* SPDX-License-Identifier: MIT. E-ink starting and status screens (layouts from art/boot/README.txt).
 * Pure functions: the host build renders the same screen to a PNG for review. */
#include "kernel.h"
#include "render.h"
#include "logo.h"
#include "font.h"
#ifndef PANEL_ROTATE_180
#define PANEL_ROTATE_180 1
#endif
/* The screen is laid out as 250x122 landscape and mapped to the 122x250 native
 * panel (16-byte rows, bit 7 first, 0 = black). PANEL_ROTATE_180=1 is the
 * orientation the second board test showed correct. */
#if PANEL_ROTATE_180
#define NATIVE_X(lx,ly) (121u-(ly))
#define NATIVE_Y(lx,ly) (lx)
#else
#define NATIVE_X(lx,ly) (ly)
#define NATIVE_Y(lx,ly) (249u-(lx))
#endif
#define SCALE 2u
#define ADV (6u*SCALE)    /* 5 columns + 1 blank column, doubled */
#define SPACE_ADV (7u*SCALE) /* a word gap is wider than a letter gap */
static void dot(uint8_t *p,unsigned lx,unsigned ly) {
 if(lx>=250||ly>=122) return;
 unsigned px=NATIVE_X(lx,ly),py=NATIVE_Y(lx,ly); p[py*16+px/8]&=(uint8_t)~(0x80>>(px%8));
}
static void draw_char(uint8_t *p,unsigned x,unsigned y,char c) {
 const uint8_t *g=font_glyph((uint8_t)c);
 for(unsigned row=0;row<7;row++) for(unsigned col=0;col<5;col++) {
  bool on=!g ? (row==0||row==6||col==0||col==4) : (g[row]>>(4-col))&1; /* unknown: a box */
  if(on&&c!=' ') for(unsigned dy=0;dy<SCALE;dy++) for(unsigned dx=0;dx<SCALE;dx++) dot(p,x+col*SCALE+dx,y+row*SCALE+dy);
 }
}
/* Layouts follow art/boot/README.txt (text boxes, 12 px advance, 14 px word gap). */
static void blit(uint8_t *p,unsigned x,unsigned y,const struct logo *l) {
 for(unsigned r=0;r<l->h;r++) for(unsigned c=0;c<l->w;c++) if(l->bits[r*l->stride+c/8]&(0x80>>(c%8))) dot(p,x+c,y+r);
}
static void plain(uint8_t *p,unsigned x,unsigned y,const char *s,unsigned max) {
 for(;*s&&max--;s++) { draw_char(p,x,y,*s); x+=*s==' '?SPACE_ADV:ADV; }
}
void render_starting(uint8_t *p) {
 memset(p,255,4000);
 blit(p,4,13,&logo_large);
 plain(p,112,24,"SANDBOX",7); plain(p,112,42,"KERNEL",6); plain(p,112,77,"STARTING...",11);
}
void render_status(uint8_t *p,const char *build_id,const char *usb,const char *error,uint32_t seconds) {
 memset(p,255,4000);
 blit(p,4,2,&logo_small);
 plain(p,54,6,"SANDBOX KERNEL",14); plain(p,54,24,build_id,12);
 char up[32]; uptime(up,seconds);
 plain(p,4,48,"UP:",3); plain(p,54,48,up,16);
 plain(p,4,70,"USB:",4); plain(p,54,70,usb,16);
 plain(p,4,92,"ERR:",4); plain(p,54,92,error,16);
}
