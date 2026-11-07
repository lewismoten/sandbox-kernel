/* SPDX-License-Identifier: MIT; original framebuffer message builder, reply
 * validator and test pattern (tag numbers from the public firmware mailbox
 * property interface documentation). No hardware access. */
#include "kernel.h"
#include "fb.h"
/* Tag order and word offsets of each tag's value buffer in the message. */
static const uint32_t tags[7][3]={ /* tag, value words, offset of first value */
 {0x48003,2,5},{0x48004,2,10},{0x48009,2,15},{0x48005,1,20},{0x48006,1,24},{0x40001,2,28},{0x40008,1,33}};
void fb_request(uint32_t m[FB_MESSAGE_WORDS],uint32_t w,uint32_t h,uint32_t d) {
 for(unsigned i=0;i<FB_MESSAGE_WORDS;i++) m[i]=0;
 m[0]=FB_MESSAGE_WORDS*4;
 for(unsigned t=0;t<7;t++) {uint32_t o=tags[t][2]; m[o-3]=tags[t][0]; m[o-2]=tags[t][1]*4; m[o-1]=tags[t][1]*4;}
 m[5]=w;m[6]=h; /* physical */ m[10]=w;m[11]=h; /* virtual */ /* offset 0,0 */
 m[20]=d; m[24]=1; /* RGB order */ m[28]=16; /* allocate, 16-byte alignment */
}
const char *fb_parse(const uint32_t m[FB_MESSAGE_WORDS],uint32_t d,struct fb_info *f) {
 if(m[0]!=FB_MESSAGE_WORDS*4||m[1]!=0x80000000u) return "FB RESP";
 for(unsigned t=0;t<7;t++) {
  uint32_t o=tags[t][2];
  if(m[o-3]!=tags[t][0]||!(m[o-1]&0x80000000u)||(m[o-1]&0x7fffffffu)<tags[t][1]*4) return "FB TAG";
 }
 uint32_t w=m[5],h=m[6];
 if(w<16||h<16||w>4096||h>4096||m[10]!=w||m[11]!=h||m[15]||m[16]) return "FB SIZE";
 if(m[20]!=d||d!=32) return "FB DEPTH";
 uint32_t pitch=m[33];
 if(pitch<w*4||pitch>w*4+4096||pitch%4) return "FB PITCH";
 uint32_t bus=m[28],bytes=m[29];
 if(!bus||!bytes) return "FB ALLOC";
 if((uint64_t)bytes<(uint64_t)pitch*h) return "FB BYTES";
 uint32_t phys=bus&0x3fffffffu;
 if(phys%16||phys<0x08000000u||phys>=0x20000000u||bytes>0x20000000u-phys) return "FB RANGE";
 f->width=w;f->height=h;f->depth=d;f->pitch=pitch;f->bytes=bytes;f->pixel_order=m[24];f->bus=bus;f->phys=phys;
 return NULL;
}
static const uint32_t bars[8]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
uint32_t fb_pattern_pixel(const struct fb_info *f,uint32_t x,uint32_t y) {
 uint32_t w=f->width,h=f->height;
 if(x<4||y<4||x>=w-4||y>=h-4) return 0xffffff;
 uint32_t c;
 if(y<h*2/3) c=bars[x*8/w];
 else {uint32_t g=(x*16/w)*17; c=(g<<16)|(g<<8)|g;}
 /* Pixel order 0 is BGR: swap so bars appear in the same order either way. */
 if(!f->pixel_order) c=((c&0xff)<<16)|(c&0xff00)|((c>>16)&0xff);
 return c;
}
void fb_pattern(const struct fb_info *f,uint8_t *pixels) {
 for(uint32_t y=0;y<f->height;y++) {
  volatile uint32_t *row=(volatile uint32_t *)(void *)(pixels+(size_t)y*f->pitch);
  for(uint32_t x=0;x<f->width;x++) row[x]=fb_pattern_pixel(f,x,y);
 }
}
