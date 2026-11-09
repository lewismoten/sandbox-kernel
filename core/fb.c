/* SPDX-License-Identifier: MIT; original framebuffer message builder, reply
 * validator and test pattern (tag numbers from the public firmware mailbox
 * property interface documentation). No hardware access. */
#include "kernel.h"
#include "fb.h"
#include "fb_art.h"
/* Tag order and word offsets of each tag's value buffer in the message. */
static const uint32_t tags[7][3]={ /* tag, value words, offset of first value */
 {0x48003,2,5},{0x48004,2,10},{0x48009,2,15},{0x48005,1,20},{0x48006,1,24},{0x40001,2,28},{0x40008,1,33}};
void fb_request(uint32_t m[FB_MESSAGE_WORDS],uint32_t w,uint32_t h,uint32_t d) {
 for(unsigned i=0;i<FB_MESSAGE_WORDS;i++) m[i]=0;
 m[0]=FB_MESSAGE_WORDS*4;
 for(unsigned t=0;t<7;t++) {uint32_t o=tags[t][2]; m[o-3]=tags[t][0]; m[o-2]=tags[t][1]*4; m[o-1]=tags[t][1]*4;}
 m[5]=w;m[6]=h; /* physical */ m[10]=w;m[11]=h; /* virtual */ /* offset 0,0 */
 m[20]=d; m[24]=FB_ORDER_BGR; /* bytes B,G,R: word 0x00RRGGBB */ m[28]=16; /* allocate, 16-byte alignment */
}
const char *fb_parse(const uint32_t m[FB_MESSAGE_WORDS],uint32_t d,struct fb_info *f) {
 if(m[0]!=FB_MESSAGE_WORDS*4||m[1]!=0x80000000u) return "FB RESP";
 for(unsigned t=0;t<7;t++) {
  uint32_t o=tags[t][2];
  if(m[o-3]!=tags[t][0]||!(m[o-1]&0x80000000u)||(m[o-1]&0x7fffffffu)<tags[t][1]*4) return "FB TAG";
 }
 uint32_t w=m[5],h=m[6];
 if(w<16||h<16||w>4096||h>4096||m[10]!=w||m[11]!=h||m[15]||m[16]) return "FB SIZE";
 if(m[20]!=d||(d!=8&&d!=16&&d!=32)) return "FB DEPTH";
 uint32_t pitch=m[33],row=w*(d/8);
 if(pitch<row||pitch>row+4096||pitch%4) return "FB PITCH";
 uint32_t bus=m[28],bytes=m[29];
 if(!bus||!bytes) return "FB ALLOC";
 if((uint64_t)bytes<(uint64_t)pitch*h) return "FB BYTES";
 uint32_t phys=bus&0x3fffffffu;
 if(phys%16||phys<0x08000000u||phys>=0x20000000u||bytes>0x20000000u-phys) return "FB RANGE";
 f->width=w;f->height=h;f->depth=d;f->pitch=pitch;f->bytes=bytes;f->pixel_order=m[24];f->bus=bus;f->phys=phys;
 return NULL;
}
uint32_t fb_palette_request(uint32_t m[FB_PALETTE_WORDS],uint32_t first,uint32_t count,const uint32_t *rgb) {
 uint32_t words=8u+count;
 m[0]=words*4u;m[1]=0;m[2]=0x4800bu;m[3]=(2u+count)*4u;m[4]=(2u+count)*4u;m[5]=first;m[6]=count;
 for(uint32_t i=0;i<count;i++) {uint32_t c=rgb[i]&0xffffffu; m[7+i]=0xff000000u|((c&0xffu)<<16)|(c&0xff00u)|(c>>16);}
 m[7+count]=0;
 return words;
}
static const uint32_t bars[8]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
uint32_t fb_pattern_pixel(const struct fb_info *f,uint32_t x,uint32_t y) {
 uint32_t w=f->width,h=f->height;
 if(x<4||y<4||x>=w-4||y>=h-4) return 0xffffff;
 uint32_t c;
 if(y<h*2/3) c=bars[x*8/w];
 else {uint32_t g=(x*16/w)*17; c=(g<<16)|(g<<8)|g;}
 /* Swapped only if the firmware kept RGB, so the bars read the same either way. */
 return fb_native(f,c);
}
/* Reviewed span inversion: floor(x*N/w) changes at ceil((k+1)*w/N).
 * Same validated 16..4096 32-bpp surfaces, forward volatile word stores,
 * borders, native pixel order and untouched row padding. 24 bounded spans;
 * no full-width stack row, framebuffer reads or mapping/barrier changes.
 * AI assistance: Hermes OpenAI Codex GPT-6.1 Sol (openai-codex). */
struct fill_span {uint32_t end,color;};
void fb_pattern(const struct fb_info *f,uint8_t *pixels) {
 static const uint32_t rgb[8]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0};
 struct fill_span bars[8],gray[16];
 const uint32_t w=f->width,h=f->height,split=h*2/3;
 for(uint32_t k=0;k<8;k++){bars[k].end=((k+1)*w+7)/8;bars[k].color=fb_native(f,rgb[k]);}
 for(uint32_t k=0;k<16;k++){gray[k].end=((k+1)*w+15)/16;gray[k].color=k*17*0x010101;}
 for(uint32_t y=0;y<h;y++) {
  volatile uint32_t *row=(volatile uint32_t *)(void *)(pixels+(size_t)y*f->pitch);
  uint32_t x=0;
  if(y<4||y>=h-4){for(;x<w;x++)row[x]=0xffffff;continue;}
  for(;x<4;x++)row[x]=0xffffff;
  const struct fill_span *spans=y<split?bars:gray;
  const uint32_t n=y<split?8:16;
  for(uint32_t k=0;k<n;k++) {
   uint32_t end=spans[k].end;if(end>w-4)end=w-4;
   for(;x<end;x++)row[x]=spans[k].color;
  }
  for(;x<w;x++)row[x]=0xffffff;
 }
}
/* Mode sets: a program's start mode, and the kernel's own screen. */
uint32_t fb_mode_us;
static bool fb_is(uint32_t w,uint32_t h,uint32_t d) {return fb0.width==w&&fb0.height==h&&fb0.depth==d;}
static bool console_up;
static uint32_t seen;
void fb_program_mode(void) {
 console_up=false;
 if(fb_pixels&&!fb_is(FB_WIDTH,FB_HEIGHT,FB_DEPTH)&&fb_set_mode(FB_WIDTH,FB_HEIGHT,FB_DEPTH)) {fb_pixels=NULL;log_line("FB program mode refused; screen off");}
}
void fb_boot_screen(const struct fb_info *f,uint8_t *pixels) {fb_pattern(f,pixels);fb_overlay(f,pixels);}
void fb_home(void) {
 console_up=false;
 if(!fb_pixels) return;
 if(!fb_is(FB_HOME_WIDTH,FB_HOME_HEIGHT,FB_HOME_DEPTH)&&fb_set_mode(FB_HOME_WIDTH,FB_HOME_HEIGHT,FB_HOME_DEPTH)) return; /* the mode it has keeps the pattern */
 fb_boot_screen(&fb0,fb_pixels);
}
/* No OS program is running: the log console in the kernel's own mode (the boot screen where the mode is not 960x540). */
void fb_recovery(void) {
 if(!fb_pixels) return;
 if(!fb_is(FB_HOME_WIDTH,FB_HOME_HEIGHT,FB_HOME_DEPTH)&&fb_set_mode(FB_HOME_WIDTH,FB_HOME_HEIGHT,FB_HOME_DEPTH)) return;
 console_up=fb_console(&fb0,fb_pixels);
 seen=console_gen();
}
void fb_recovery_task(void) { /* redraws the log box when a line has been added since the last draw */
 if(!console_up||!fb_pixels||console_gen()==seen) return;
 seen=console_gen();fb_console_log(&fb0,fb_pixels);
}
