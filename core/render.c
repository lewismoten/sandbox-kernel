/* SPDX-License-Identifier: MIT. Original 5x7 bitmap font and status layout.
 * Pure functions: the host build renders the same screen to a PNG for review. */
#include "kernel.h"
#include "render.h"
#include "logo.h"
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
#define LINE 16u
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:./";
/* Each glyph: 7 rows, 5 bits, bit 4 = leftmost column. Drawn for this project. */
static const uint8_t glyphs[][7]={
{014,021,021,037,021,021,021}, /* A */ {036,021,021,036,021,021,036}, /* B */
{016,021,020,020,020,021,016}, /* C */ {036,021,021,021,021,021,036}, /* D */
{037,020,020,036,020,020,037}, /* E */ {037,020,020,036,020,020,020}, /* F */
{016,021,020,027,021,021,017}, /* G */ {021,021,021,037,021,021,021}, /* H */
{016,004,004,004,004,004,016}, /* I */ {007,002,002,002,002,022,014}, /* J */
{021,022,024,030,024,022,021}, /* K */ {020,020,020,020,020,020,037}, /* L */
{021,033,025,025,021,021,021}, /* M */ {021,031,025,025,023,021,021}, /* N */
{016,021,021,021,021,021,016}, /* O */ {036,021,021,036,020,020,020}, /* P */
{016,021,021,021,025,022,015}, /* Q */ {036,021,021,036,024,022,021}, /* R */
{017,020,020,016,001,001,036}, /* S */ {037,004,004,004,004,004,004}, /* T */
{021,021,021,021,021,021,016}, /* U */ {021,021,021,021,021,012,004}, /* V */
{021,021,021,025,025,033,021}, /* W */ {021,021,012,004,012,021,021}, /* X */
{021,021,012,004,004,004,004}, /* Y */ {037,001,002,004,010,020,037}, /* Z */
{016,021,023,025,031,021,016}, /* 0 */ {004,014,004,004,004,004,016}, /* 1 */
{016,021,001,002,004,010,037}, /* 2 */ {036,001,001,016,001,001,036}, /* 3 */
{002,006,012,022,037,002,002}, /* 4 */ {037,020,036,001,001,021,016}, /* 5 */
{006,010,020,036,021,021,016}, /* 6 */ {037,001,002,004,010,010,010}, /* 7 */
{016,021,021,016,021,021,016}, /* 8 */ {016,021,021,017,001,002,014}, /* 9 */
{000,000,000,037,000,000,000}, /* - */ {000,004,004,000,004,004,000}, /* : */
{000,000,000,000,000,014,014}, /* . */ {001,001,002,004,010,020,020}, /* / */
};
static void dot(uint8_t *p,unsigned lx,unsigned ly) {
 if(lx>=250||ly>=122) return;
 unsigned px=NATIVE_X(lx,ly),py=NATIVE_Y(lx,ly); p[py*16+px/8]&=(uint8_t)~(0x80>>(px%8));
}
static unsigned glyph_index(unsigned ch) {
 if(ch>='a'&&ch<='z') ch-=32;
 for(unsigned i=0;i<sizeof(alphabet)-1;i++) if(ch==(unsigned)alphabet[i]) return i;
 return 0xffffu;
}
static unsigned width(const char *s,size_t n) { unsigned w=0; while(n--) w+=*s++==' '?SPACE_ADV:ADV; return w; }
static void draw_char(uint8_t *p,unsigned x,unsigned y,char c) {
 unsigned i=glyph_index((uint8_t)c);
 for(unsigned row=0;row<7;row++) for(unsigned col=0;col<5;col++) {
  bool on=i==0xffffu ? (row==0||row==6||col==0||col==4) : (glyphs[i][row]>>(4-col))&1; /* unknown: a box */
  if(on&&c!=' ') for(unsigned dy=0;dy<SCALE;dy++) for(unsigned dx=0;dx<SCALE;dx++) dot(p,x+col*SCALE+dx,y+row*SCALE+dy);
 }
}
/* Draws "label value" with the value wrapped at word boundaries under itself. */
static unsigned field(uint8_t *p,unsigned y,const char *label,const char *value) {
 unsigned x0=0; for(const char *l=label;*l;l++) x0+=ADV; 
 for(unsigned i=0;label[i]&&y+7*SCALE<=122;i++) draw_char(p,width(label,i)+4,y,label[i]);
 x0+=4+SPACE_ADV; unsigned x=x0;
 while(*value) {
  size_t n=0; while(value[n]&&value[n]!=' ') n++;
  if(x>x0&&x+width(value,n)>246) { y+=LINE; x=x0; }
  if(y+7*SCALE>122) return y;
  for(size_t i=0;i<n;i++) { if(x+ADV>248) { y+=LINE; x=x0; } draw_char(p,x,y,value[i]); x+=ADV; }
  value+=n; while(*value==' ') { value++; x+=SPACE_ADV; }
 }
 return y+LINE;
}
static void plain(uint8_t *p,unsigned x,unsigned y,const char *s) { for(;*s;s++) { draw_char(p,x,y,*s); x+=*s==' '?SPACE_ADV:ADV; } }
static void blit(uint8_t *p,unsigned x,unsigned y,const struct logo *l) {
 for(unsigned r=0;r<l->h;r++) for(unsigned c=0;c<l->w;c++) if(l->bits[r*l->stride+c/8]&(0x80>>(c%8))) dot(p,x+c,y+r);
}
void render_starting(uint8_t *p) {
 memset(p,255,4000);
 blit(p,4,(122-logo_large.h)/2,&logo_large); /* round badge fills the panel's height */
 unsigned x=logo_large.w+12;
 plain(p,x,26,"GAME-MAKER"); plain(p,x,42,"OS"); plain(p,x,76,"STARTING...");
}
void render_status(uint8_t *p,const char *build_id,const char *usb,const char *error,uint32_t seconds) {
 memset(p,255,4000);
 blit(p,4,2,&logo_small);
 plain(p,logo_small.w+10,4,"GAME-MAKER"); plain(p,logo_small.w+10,4+LINE,build_id);
 char up[32]; uptime(up,seconds);
 unsigned y=44;
 y=field(p,y,"UP:",up); y=field(p,y,"USB:",usb); field(p,y,"ERR:",error);
}
