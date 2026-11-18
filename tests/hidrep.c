/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * A descriptor and reports built from the facts read off the Rii receiver's interface 1 (report IDs
 * 1 mouse, 2 system control, 3 consumer control). Not a capture: the real descriptor bytes are not
 * in hand yet; replace DESC with them when they are. */
#include "hidrep.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const uint8_t DESC[]={
 0x05,1,0x09,2,0xa1,1,0x85,1,0x09,1,0xa1,0,
  0x05,9,0x19,1,0x29,5,0x15,0,0x25,1,0x75,1,0x95,5,0x81,2, 0x75,3,0x95,1,0x81,3,
  0x05,1,0x09,0x30,0x09,0x31,0x16,0x01,0x80,0x26,0xff,0x7f,0x75,16,0x95,2,0x81,6,
  0x09,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6, 0xc0,0xc0,
 0x05,1,0x09,0x80,0xa1,1,0x85,2,0x19,0x81,0x29,0x83,0x15,0,0x25,1,0x75,1,0x95,3,0x81,2,0x75,5,0x95,1,0x81,3,0xc0,
 0x05,0x0c,0x09,1,0xa1,1,0x85,3,0x19,0,0x2a,0xff,3,0x15,0,0x26,0xff,3,0x75,16,0x95,1,0x81,0,0xc0};
static void valid(void) {
 struct hidrep h;struct hidrep_out o;
 assert(hidrep_parse(DESC,sizeof DESC,&h)==0&&h.uses_ids);
 uint8_t m[]={1,0x05,0xfb,0xff,0x10,0x00,0xff};   /* buttons 1+3, dx -5, dy +16, wheel -1 */
 assert(hidrep_decode(&h,m,sizeof m,&o)==0&&o.id==1&&o.buttons==5&&o.dx==-5&&o.dy==16&&o.wheel==-1);
 uint8_t m0[]={1,0,0,0,0,0,0};assert(hidrep_decode(&h,m0,sizeof m0,&o)==0&&o.buttons==0&&o.dx==0);
 uint8_t sl[]={2,2};assert(hidrep_decode(&h,sl,2,&o)==0&&o.id==2&&o.system[0]==0x82&&!o.system[1]);
 uint8_t vu[]={3,0xe9,0};assert(hidrep_decode(&h,vu,3,&o)==0&&o.consumer[0]==0xe9);  /* volume up */
 uint8_t pp[]={3,0xcd,0};assert(hidrep_decode(&h,pp,3,&o)==0&&o.consumer[0]==0xcd);  /* play/pause */
 uint8_t up[]={3,0,0};assert(hidrep_decode(&h,up,3,&o)==0&&!o.consumer[0]);
 assert(hidrep_decode(&h,m,3,&o)==-1);            /* short */
 uint8_t bad[]={9,0,0};assert(hidrep_decode(&h,bad,3,&o)==-1);   /* unknown ID */
 for(unsigned n=0;n<sizeof DESC;n++) hidrep_parse(DESC,n,&h);    /* a cut descriptor never crashes */
 assert(hidrep_parse(0,0,&h)==-1);
}

/* Independent short-item framing: 0x97's size bits 3 mean FOUR count
 * bytes; 0x95's size bits 1 mean ONE. Corrected huge-count fixtures below
 * are not the historical report's incorrectly prefixed examples. */
static void huge(unsigned zero) {
 struct hidrep h;
 uint8_t d[]={0x97,0xff,0xff,0xff,0xff,0x75,1,0x81,2};
 assert((d[0]&3u)==3u&&((d[0]>>2)&3u)==1u&&(d[0]>>4)==9u);
 uint32_t count=(uint32_t)d[1]|((uint32_t)d[2]<<8)|((uint32_t)d[3]<<16)|((uint32_t)d[4]<<24);
 assert(count==UINT32_MAX);
 d[6]=(uint8_t)(zero?0:1);
 assert(hidrep_parse(d,sizeof d,&h)==-1);
 if(zero) {
  d[3]=d[4]=0; /* 65535 zero-size iterations are the inclusive work cap */
  assert(hidrep_parse(d,sizeof d,&h)==0&&h.n==0);
  d[1]=d[2]=0;d[3]=1; /* 65536 is rejected even with size zero */
  assert(hidrep_parse(d,sizeof d,&h)==-1);
 }
}
static void bits(void) {
 struct hidrep h;
 /* Independent LE count decoding: 0x00011170 == 70000. */
 const uint8_t reported[]={0x97,0x70,0x11,0x01,0,0x75,1,0x81,2,0x81,2};
 assert(((uint32_t)reported[1]|((uint32_t)reported[2]<<8)|((uint32_t)reported[3]<<16))==70000u);
 assert(hidrep_parse(reported,sizeof reported,&h)==-1);
 /* Count alone fits; the widened count * size must also fit (padding). */
 const uint8_t product[]={0x96,0x40,0x9c,0x75,2,0x81,3}; /* 40000 * 2 */
 assert(hidrep_parse(product,sizeof product,&h)==-1);
 /* Exact bit boundary and one over, constant padding consumes position. */
 uint8_t cumulative[]={0x96,0xef,0xff,0x75,1,0x81,3,
                        0x05,1,0x09,0x30,0x95,1,0x75,16,0x81,2};
 assert(hidrep_parse(cumulative,sizeof cumulative,&h)==0&&h.n==1&&h.f[0].off==65519);
 cumulative[1]=0xf0; /* 65520 + 16 = 65536; no wrapping/narrowing */
 assert(hidrep_parse(cumulative,sizeof cumulative,&h)==-1);
}
static void id_positions(void) {
 struct hidrep h;
 uint8_t d[]={0x85,1,0x96,0xef,0xff,0x75,1,0x81,3,
              0x85,2,0x95,1,0x75,16,0x09,0x30,0x81,2,
              0x85,1,0x95,1,0x75,16,0x09,0x31,0x81,2};
 assert(hidrep_parse(d,sizeof d,&h)==0&&h.n==2);
 assert(h.f[0].id==2&&h.f[0].off==0&&h.f[1].id==1&&h.f[1].off==65519);
 d[3]=0xf0; /* returning to ID 1 must resume its existing bit position */
 assert(hidrep_parse(d,sizeof d,&h)==-1);
}
static void zero_logical(unsigned maximum) {
 struct hidrep h;struct hidrep_out o;
 uint8_t d[]={0x05,1,0x09,0x30,0x15,0xff,0x25,0x7f,0x14,0x75,8,0x95,1,0x81,2};
 d[8]=(uint8_t)(maximum?0x24:0x14);
 assert(hidrep_parse(d,sizeof d,&h)==0&&h.n==1);
 assert(h.f[0].lmin==(maximum?-1:0));
 const uint8_t r[]={0xff};
 assert(hidrep_decode(&h,r,sizeof r,&o)==0&&o.dx==(maximum?-1:255));
}
static void fields(void) {
 struct hidrep h;struct hidrep_out o;
 uint8_t d[]={0x05,9,0x19,1,0x29,32,0x75,1,0x95,24,0x81,2};
 const uint8_t r[]={0xff,0xff,0xff};
 assert(hidrep_parse(d,sizeof d,&h)==0&&h.n==24);
 assert(hidrep_decode(&h,r,sizeof r,&o)==0&&o.buttons==0xffffffu);
 d[9]=25;assert(hidrep_parse(d,sizeof d,&h)==-1);
 /* The field cap is cumulative across Input items, not just each count. */
 uint8_t split[]={0x05,9,0x19,1,0x29,32,0x75,1,0x95,12,0x81,2,
                  0x95,12,0x81,2,0x95,3,0x81,3};
 assert(hidrep_parse(split,sizeof split,&h)==0&&h.n==24); /* padding is not a field */
 split[13]=13;assert(hidrep_parse(split,sizeof split,&h)==-1);
}
static void ids(void) {
 struct hidrep h;struct hidrep_out o;
 uint8_t d[9*12];
 for(unsigned i=0;i<9;i++) {
  const uint8_t one[]={0x85,(uint8_t)(i+1),0x05,9,0x09,1,0x75,1,0x95,1,0x81,2};
  memcpy(d+i*sizeof one,one,sizeof one);
 }
 assert(hidrep_parse(d,8*12,&h)==0&&h.n==8);
 for(unsigned i=1;i<=8;i++) {
  const uint8_t r[]={(uint8_t)i,1};
  assert(hidrep_decode(&h,r,sizeof r,&o)==0&&o.id==i&&o.buttons==1);
 }
 assert(hidrep_parse(d,sizeof d,&h)==-1);
}
static void unsupported(void) {
 struct hidrep h;
 const uint8_t push[]={0xa4},pop[]={0xb4};
 const uint8_t usage[]={0x0b,0xe9,0,0x0c,0};
 const uint8_t range_min[]={0x1b,0xe0,0,0x0c,0};
 const uint8_t range_max[]={0x2b,0xef,0,0x0c,0};
 const uint8_t page[]={0x07,0,0,1,0};
 assert(hidrep_parse(push,sizeof push,&h)==-1);
 assert(hidrep_parse(pop,sizeof pop,&h)==-1);
 assert(hidrep_parse(usage,sizeof usage,&h)==-1);
 assert(hidrep_parse(range_min,sizeof range_min,&h)==-1);
 assert(hidrep_parse(range_max,sizeof range_max,&h)==-1);
 assert(hidrep_parse(page,sizeof page,&h)==-1);
}
static void active_limit(void) {
 struct hidrep h;struct hidrep_out o;
 const uint8_t d[]={0x05,0x0c,0x19,0xe0,0x29,0xe4,0x75,8,0x95,5,0x81,2};
 const uint8_t r[]={1,1,1,1,1};
 assert(hidrep_parse(d,sizeof d,&h)==0&&h.n==5);
 assert(hidrep_decode(&h,r,sizeof r,&o)==0);
 for(unsigned i=0;i<HIDREP_ACTIVE;i++) assert(o.consumer[i]==0xe0u+i);
}
int main(int argc,char **argv) {
 valid(); /* actual valid signed mouse/system/consumer decode controls */
 if(argc>1) {
  if(!strcmp(argv[1],"huge")) huge(0);
  else if(!strcmp(argv[1],"huge-zero")) huge(1);
  else if(!strcmp(argv[1],"bits")) bits();
  else if(!strcmp(argv[1],"id-positions")) id_positions();
  else if(!strcmp(argv[1],"zero-min")) zero_logical(0);
  else if(!strcmp(argv[1],"zero-max")) zero_logical(1);
  else if(!strcmp(argv[1],"fields")) fields();
  else if(!strcmp(argv[1],"ids")) ids();
  else if(!strcmp(argv[1],"unsupported")) unsupported();
  else if(!strcmp(argv[1],"active-limit")) active_limit();
  else return 2;
 }
 puts("hidrep controls and selected bounds: OK");
 return 0;
}
