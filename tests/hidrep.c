/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * A descriptor and reports built from the facts read off the Rii receiver's interface 1 (report IDs
 * 1 mouse, 2 system control, 3 consumer control). Not a capture: the real descriptor bytes are not
 * in hand yet; replace DESC with them when they are. */
#include "hidrep.h"
#include <assert.h>
static const uint8_t DESC[]={
 0x05,1,0x09,2,0xa1,1,0x85,1,0x09,1,0xa1,0,
  0x05,9,0x19,1,0x29,5,0x15,0,0x25,1,0x75,1,0x95,5,0x81,2, 0x75,3,0x95,1,0x81,3,
  0x05,1,0x09,0x30,0x09,0x31,0x16,0x01,0x80,0x26,0xff,0x7f,0x75,16,0x95,2,0x81,6,
  0x09,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6, 0xc0,0xc0,
 0x05,1,0x09,0x80,0xa1,1,0x85,2,0x19,0x81,0x29,0x83,0x15,0,0x25,1,0x75,1,0x95,3,0x81,2,0x75,5,0x95,1,0x81,3,0xc0,
 0x05,0x0c,0x09,1,0xa1,1,0x85,3,0x19,0,0x2a,0xff,3,0x15,0,0x26,0xff,3,0x75,16,0x95,1,0x81,0,0xc0};
int main(void) {
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
 return 0;
}
