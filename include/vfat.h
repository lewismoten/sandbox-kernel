/* SPDX-License-Identifier: MIT; bounded VFAT name decoding, no allocation. */
#pragma once
#include "kernel.h"
struct vfat_name {uint16_t units[260];uint8_t next,count,sum;};
static void vfat_reset(struct vfat_name *n) {n->next=0;n->count=0;n->sum=0;}
static uint8_t vfat_checksum(const uint8_t *e) {
 uint8_t s=0;for(unsigned i=0;i<11;i++)s=(uint8_t)((s>>1)|((s&1)<<7)),s=(uint8_t)(s+e[i]);return s;
}
static void vfat_slot(struct vfat_name *n,const uint8_t *e) {
 static const uint8_t offsets[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
 unsigned ord=e[0]&31;
 if(e[11]!=15||e[12]||e[26]||e[27]||(e[0]&0xa0)||!ord||ord>20) {vfat_reset(n);return;}
 if(e[0]&0x40) {n->count=(uint8_t)ord;n->next=(uint8_t)ord;n->sum=e[13];}
 if(!n->next||ord!=n->next||e[13]!=n->sum) {vfat_reset(n);return;}
 for(unsigned i=0;i<13;i++)n->units[(ord-1)*13+i]=(uint16_t)(e[offsets[i]]|(e[offsets[i]+1]<<8));
 n->next--;
}
static bool vfat_char(uint32_t c) {
 if(c<32||c==127)return false;
 const char *bad="\"*/:<>?\\|";for(;*bad;bad++)if(c==(uint8_t)*bad)return false;
 return true;
}
/* A complete valid name must also fit the existing 255-byte UTF-8 ABI. Longer
 * on-disk names remain accessible by their short aliases; never truncate. */
static bool vfat_decode(const struct vfat_name *n,const uint8_t *e,char out[256]) {
 if(!n->count||n->next||n->sum!=vfat_checksum(e))return false;
 unsigned total=n->count*13,len=total,k=0;
 for(unsigned i=0;i<total;i++)if(n->units[i]==0) {len=i;for(unsigned j=i+1;j<total;j++)if(n->units[j]!=0xffff)return false;break;}
 if(!len||len>255||(len+12)/13!=n->count)return false;
 if(n->units[len-1]==' '||n->units[len-1]=='.')return false;
 for(unsigned i=0;i<len;i++) {
  uint32_t c=n->units[i];
  if(c>=0xd800&&c<=0xdbff) {if(++i>=len||n->units[i]<0xdc00||n->units[i]>0xdfff)return false;c=0x10000+((c-0xd800)<<10)+(n->units[i]-0xdc00);}
  else if(c>=0xdc00&&c<=0xdfff)return false;
  if(c==0xffff||!vfat_char(c))return false;
  unsigned bytes=c<0x80?1:c<0x800?2:c<0x10000?3:4;if(k+bytes>255)return false;
  if(bytes==1)out[k++]=(char)c;
  else {
   out[k++]=(char)((bytes==2?0xc0:bytes==3?0xe0:0xf0)|(c>>(6*(bytes-1))));
   for(unsigned j=bytes-1;j;j--)out[k++]=(char)(0x80|((c>>(6*(j-1)))&63));
  }
 }
 out[k]=0;return true;
}
/* Strict UTF-8 input; reject overlong encodings, surrogates and > U+10FFFF. */
static uint32_t vfat_utf8(const char *p,unsigned n,unsigned *at) {
 unsigned i=*at;uint32_t c=(uint8_t)p[i++];unsigned more=0;uint32_t min=0;
 if(c>=0x80) {
  if(c>=0xc2&&c<=0xdf){c&=31;more=1;min=0x80;}
  else if(c>=0xe0&&c<=0xef){c&=15;more=2;min=0x800;}
  else if(c>=0xf0&&c<=0xf4){c&=7;more=3;min=0x10000;}
  else return ~0u;
  if(more>n-i)return ~0u;
  while(more--) {uint8_t b=(uint8_t)p[i++];if((b&0xc0)!=0x80)return ~0u;c=(c<<6)|(b&63);}
  if(c<min||(c>=0xd800&&c<=0xdfff)||c>0x10ffff)return ~0u;
 }
 *at=i;return c;
}
#include "vfat_fold.h"
static bool vfat_equal(const char *p,unsigned n,const char *name) {
 unsigned a=0,b=0,m=(unsigned)strlen(name);
 while(a<n&&b<m)if(vfat_fold(vfat_utf8(p,n,&a))!=vfat_fold(vfat_utf8(name,m,&b)))return false;
 return a==n&&b==m;
}
static bool vfat_component(const char *p,unsigned n) {
 if(!n||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.'))return false;
 unsigned at=0;while(at<n){uint32_t c=vfat_utf8(p,n,&at);if(c==~0u||!vfat_char(c))return false;}
 return true;
}
