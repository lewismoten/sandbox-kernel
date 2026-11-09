/* SPDX-License-Identifier: MIT
 * Production host oracle regression; Hermes OpenAI Codex GPT-6.1 Sol. */
#define _POSIX_C_SOURCE 200809L
#include "kernel.h"
#include "fb.h"
static void reference_pattern(const struct fb_info *f,uint8_t *pixels) {
 for(uint32_t y=0;y<f->height;y++) {
  volatile uint32_t *row=(volatile uint32_t *)(void *)(pixels+(size_t)y*f->pitch);
  for(uint32_t x=0;x<f->width;x++)row[x]=fb_pattern_pixel(f,x,y);
 }
}
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
static uint32_t oracle(uint32_t w,uint32_t h,uint32_t order,uint32_t x,uint32_t y) {
 static const uint32_t rgb[8]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0};
 if(x<4||y<4||x+4>=w||y+4>=h)return 0xffffff;
 uint32_t c;
 if(y<h*2/3) c=rgb[(uint64_t)x*8/w];
 else {uint32_t v=((uint64_t)x*16/w)*17;c=v*0x010101;}
 if(order)c=((c&255)<<16)|(c&0xff00)|(c>>16);
 return c;
}
static uint32_t rng=0x2420cf6f;
static uint32_t random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void check(uint32_t w,uint32_t h,uint32_t pad,uint32_t order) {
 struct fb_info f={w,h,32,w*4+pad,(w*4+pad)*h,order,0,0};
 size_t n=f.bytes+128;uint8_t *a=malloc(n),*b=malloc(n);assert(a&&b);
 memset(a,0xa5,n);memset(b,0xa5,n);
 reference_pattern(&f,a+64);fb_pattern(&f,b+64);
 if(memcmp(a,b,n)){fprintf(stderr,"MISMATCH w=%u h=%u pitch=%u order=%u\n",w,h,f.pitch,order);exit(1);}
 for(uint32_t y=0;y<h;y++) {
  uint32_t *p=(uint32_t *)(void *)(b+64+(size_t)y*f.pitch);
  for(uint32_t x=0;x<w;x++)assert(p[x]==oracle(w,h,order,x,y));
  for(uint32_t k=w*4;k<f.pitch;k++)assert(b[64+(size_t)y*f.pitch+k]==0xa5);
 }
 for(unsigned i=0;i<64;i++)assert(b[i]==0xa5&&b[n-1-i]==0xa5);
 free(a);free(b);
}
int main(void){
 unsigned cases=0;
 check(960,540,0,0);cases++;
 for(uint32_t w=16;w<=257;w++)for(uint32_t order=0;order<2;order++){check(w,16+w%29,(w%9)*4,order);cases++;}
 for(unsigned i=0;i<100;i++){uint32_t w=16+random32()%4081,h=16+random32()%113;check(w,h,(random32()%1025)*4,random32()&1);cases++;}
 check(4096,4096,4096,1);cases++;
 check(1920,1080,64,1);cases++;
 printf("PASS %u surfaces: exact production/oracle pixels, padding and allocation guards\n",cases);return 0;
}
