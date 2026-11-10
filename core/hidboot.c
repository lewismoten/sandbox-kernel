/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * HID Usage Tables 1.x keyboard page (7) to Linux key codes, written from the published tables. */
#include "hidboot.h"
#include <string.h>

static const uint16_t mods[8]={29,42,56,125,97,54,100,126};  /* L ctrl shift alt gui, R ctrl shift alt gui */
static const uint8_t letters[26]={30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
static const uint8_t punct[]={28,1,14,15,57,12,13,26,27,43,43,39,40,41,51,52,53,58}; /* 0x28..0x39 */
static const uint8_t nav[]={99,70,119,110,102,104,111,107,109,106,105,108,103,69,98,55,74,78,96}; /* 0x46..0x58 */
static const uint8_t pad[]={79,80,81,75,76,77,71,72,73,82,83};  /* 0x59..0x63: 1..9, 0, . */

uint16_t hidboot_linux_key(uint8_t u) {
 if(u>=0x04&&u<=0x1d) return letters[u-0x04];
 if(u>=0x1e&&u<=0x26) return (uint16_t)(2+u-0x1e);
 if(u==0x27) return 11;
 if(u>=0x28&&u<=0x39) return punct[u-0x28];
 if(u>=0x3a&&u<=0x43) return (uint16_t)(59+u-0x3a);
 if(u==0x44) return 87;
 if(u==0x45) return 88;
 if(u>=0x46&&u<=0x58) return nav[u-0x46];
 if(u>=0x59&&u<=0x63) return pad[u-0x59];
 return 0;
}
void hidboot_init(struct hidboot *s) {memset(s->prev,0,sizeof s->prev);}

/* Held means some usage in the report maps to Linux key k (two usages can share one key). */
static int held(const uint8_t *r,uint16_t k) {
 for(unsigned i=2;i<HIDBOOT_REPORT;i++) if(r[i]>=4&&hidboot_linux_key(r[i])==k) return 1;
 return 0;
}
int hidboot_diff(struct hidboot *s,const uint8_t *rep,uint32_t len,struct input_change *out) {
 if(!s||!rep||!out||len!=HIDBOOT_REPORT) return -1;
 for(unsigned i=2;i<HIDBOOT_REPORT;i++) if(rep[i]==0x01) return 0;   /* ErrorRollOver */
 int n=0;
 for(unsigned i=2;i<HIDBOOT_REPORT;i++) {                            /* releases */
  uint8_t u=s->prev[i];uint16_t k;
  if(u<4||!(k=hidboot_linux_key(u))||held(rep,k)) continue;
  int dup=0;for(unsigned j=2;j<i;j++) if(s->prev[j]>=4&&hidboot_linux_key(s->prev[j])==k) dup=1;
  if(dup) continue;
  out[n].type=EV_KEY;out[n].code=k;out[n].value=0;n++;
 }
 for(unsigned b=0;b<8;b++) {
  unsigned was=(s->prev[0]>>b)&1u,now=(rep[0]>>b)&1u;
  if(was&&!now) {out[n].type=EV_KEY;out[n].code=mods[b];out[n].value=0;n++;}
 }
 for(unsigned b=0;b<8;b++) {
  unsigned was=(s->prev[0]>>b)&1u,now=(rep[0]>>b)&1u;
  if(!was&&now) {out[n].type=EV_KEY;out[n].code=mods[b];out[n].value=1;n++;}
 }
 for(unsigned i=2;i<HIDBOOT_REPORT;i++) {                            /* presses */
  uint8_t u=rep[i];uint16_t k;
  if(u<4||!(k=hidboot_linux_key(u))||held(s->prev,k)) continue;
  int dup=0;for(unsigned j=2;j<i;j++) if(rep[j]>=4&&hidboot_linux_key(rep[j])==k) dup=1;
  if(dup) continue;
  out[n].type=EV_KEY;out[n].code=k;out[n].value=1;n++;
 }
 memcpy(s->prev,rep,HIDBOOT_REPORT);
 return n;
}
int hidboot_feed(struct hidboot *s,const uint8_t *rep,uint32_t len) {
 struct input_change c[HIDBOOT_CHANGES_MAX];
 int n=hidboot_diff(s,rep,len,c);
 if(n<0) return -22;   /* -EINVAL */
 for(int i=0;i<n;i+=(int)INPUT_FRAME_MAX) {
  int m=n-i>(int)INPUT_FRAME_MAX?(int)INPUT_FRAME_MAX:n-i;
  int r=input_frame(c+i,(unsigned)m);
  if(r) return r;
 }
 return 0;
}
void hidboot_held(const uint8_t *rep,uint8_t out[16]) {
 for(unsigned i=0;i<16;i++) out[i]=0;
 for(unsigned b=0;b<8;b++) if((rep[0]>>b)&1u) out[mods[b]>>3]|=(uint8_t)(1u<<(mods[b]&7));
 for(unsigned i=2;i<HIDBOOT_REPORT;i++) {uint16_t k=rep[i]>=4?hidboot_linux_key(rep[i]):0;if(k&&k<128) out[k>>3]|=(uint8_t)(1u<<(k&7));}
}
int hidboot_release_all(struct hidboot *s) {
 static const uint8_t zero[HIDBOOT_REPORT]={0};
 return hidboot_feed(s,zero,HIDBOOT_REPORT);
}
