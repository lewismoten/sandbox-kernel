/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * HID 1.11 report descriptor items (section 6.2.2), read from the published spec. */
#include "hidrep.h"
#include <string.h>

#define MAXID 8
static int32_t sext(uint32_t v,unsigned n) {return n>=32?(int32_t)v:(int32_t)(v<<(32-n))>>(32-n);}

int hidrep_parse(const uint8_t *d,uint32_t len,struct hidrep *h) {
 if(!d||!h) return -1;
 memset(h,0,sizeof *h);
 uint16_t page=0,usages[HIDREP_FIELDS],umin=0,umax=0;unsigned nu=0,have_range=0;
 int32_t lmin=0,lmax=0;uint32_t size=0,count=0;uint8_t id=0;
 uint8_t ids[MAXID]={0},used[MAXID]={0};uint16_t pos[MAXID]={0};
 for(uint32_t i=0;i<len;) {
  uint8_t b=d[i];
  if(b==0xfe) {if(i+2>len) return -1; i+=3u+d[i+1]; if(i>len) return -1; continue;}  /* long item */
  unsigned sz=b&3u;if(sz==3) sz=4;
  if(i+1+sz>len) return -1;
  uint32_t v=0;for(unsigned k=0;k<sz;k++) v|=(uint32_t)d[i+1+k]<<(8*k);
  unsigned type=(b>>2)&3u,tag=b>>4;
  i+=1+sz;
  if(type==1) {           /* global */
   if(tag==0) page=(uint16_t)v; else if(tag==1) lmin=sext(v,8*sz); else if(tag==2) lmax=sext(v,8*sz);
   else if(tag==7) size=v; else if(tag==9) count=v;
   else if(tag==8) {id=(uint8_t)v;h->uses_ids=1;}
  } else if(type==2) {    /* local */
   if(tag==0) {if(nu<HIDREP_FIELDS) usages[nu++]=(uint16_t)v;}
   else if(tag==1) {umin=(uint16_t)v;have_range=1;} else if(tag==2) umax=(uint16_t)v;
  } else if(type==0 && tag==8) {   /* Input */
   unsigned slot=0;while(slot<MAXID&&used[slot]&&ids[slot]!=id) slot++;
   if(slot==MAXID) return -1;
   ids[slot]=id;used[slot]=1;
   if(size>32) return -1;
   for(uint32_t c=0;c<count;c++) {
    if(!(v&1u)&&h->n<HIDREP_FIELDS&&size>0) {
     struct hidrep_field *f=&h->f[h->n++];
     f->id=id;f->bits=(uint8_t)size;f->off=pos[slot];f->page=page;f->lmin=lmin;
     f->sign=lmin<0;f->array=(v&2u)==0;
     if(f->array) f->usage=have_range?umin:(nu?usages[0]:0);
     else f->usage=c<nu?usages[c]:(have_range&&umin+c<=umax?(uint16_t)(umin+c):(nu?usages[nu-1]:0));
    }
    pos[slot]=(uint16_t)(pos[slot]+size);
   }
   (void)lmax;
  }
  if(type==0) {nu=0;have_range=0;umin=umax=0;}   /* main items clear the locals */
 }
 return 0;
}

static void put(uint16_t *a,uint16_t u) {
 for(unsigned i=0;i<HIDREP_ACTIVE;i++) if(!a[i]) {a[i]=u;return;}
}
int hidrep_decode(const struct hidrep *h,const uint8_t *r,uint32_t len,struct hidrep_out *o) {
 if(!h||!r||!o||len==0) return -1;
 memset(o,0,sizeof *o);
 uint32_t base=0;
 if(h->uses_ids) {o->id=r[0];base=8;}
 unsigned seen=0;
 for(unsigned i=0;i<h->n;i++) {
  const struct hidrep_field *f=&h->f[i];
  if(f->id!=o->id) continue;
  seen=1;
  uint32_t bit=base+f->off;
  if(bit+f->bits>len*8u) return -1;
  uint32_t v=0;
  for(unsigned k=0;k<f->bits;k++) v|=(uint32_t)((r[(bit+k)>>3]>>((bit+k)&7u))&1u)<<k;
  int32_t sv=f->sign?sext(v,f->bits):(int32_t)v;
  if(f->page==9) {if(v&&f->usage>=1&&f->usage<=32&&!f->array) o->buttons|=1u<<(f->usage-1);}
  else if(f->page==1&&!f->array) {
   if(f->usage==0x30) o->dx+=sv; else if(f->usage==0x31) o->dy+=sv; else if(f->usage==0x38) o->wheel+=sv;
   else if(v&&f->usage>=0x81&&f->usage<=0x83) put(o->system,f->usage);
  } else if(f->page==0x0c&&sv) {
   if(f->array) put(o->consumer,(uint16_t)(sv-f->lmin+f->usage)); else put(o->consumer,f->usage);
  } else if(f->page==1&&f->array&&sv) put(o->system,(uint16_t)(sv-f->lmin+f->usage));
 }
 return seen?0:-1;
}
