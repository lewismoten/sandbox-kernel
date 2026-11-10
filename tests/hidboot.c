/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usbdesc.h"
#include "hidboot.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>

static struct input_change got[64];static unsigned ngot,frames;
int input_frame(const struct input_change *c,unsigned n) {
 assert(n<=INPUT_FRAME_MAX);memcpy(got+ngot,c,n*sizeof *c);ngot+=n;frames++;return 0;
}
#define CFG(len) 9,2,(len)&255,(len)>>8,3,1,0,0xa0,50
#define IF(n,sub,pr) 9,4,(n),0,1,3,(sub),(pr),0
#define HID 9,0x21,0x11,1,0,1,0x22,63,0
#define EP(a,mps,iv) 7,5,(a),3,(mps),0,(iv)
/* keyboard first, then mouse, then consumer control (a typical 2.4 GHz dongle) */
static const uint8_t kb_first[]={CFG(0),
 IF(0,1,1),HID,EP(0x81,8,10),IF(1,1,2),HID,EP(0x82,8,10),IF(2,0,0),HID,EP(0x83,4,2)};
static const uint8_t kb_second[]={CFG(0),IF(0,1,2),HID,EP(0x81,4,10),IF(1,1,1),HID,EP(0x82,8,8),IF(2,0,0),HID,EP(0x83,4,2)};
static uint8_t *copy(const uint8_t *d,unsigned n,unsigned fixlen) {
 uint8_t *b=malloc(n);memcpy(b,d,n);if(fixlen){b[2]=n&255;b[3]=n>>8;}return b;
}
static void down_up(void) {
 struct hidboot s;hidboot_init(&s);ngot=0;
 uint8_t r1[8]={0,0,0x52};                     /* up arrow */
 assert(hidboot_feed(&s,r1,8)==0&&ngot==1&&got[0].code==103&&got[0].value==1);
 uint8_t r2[8]={2,0,0x52,0x2c};                /* + left shift, space */
 ngot=0;assert(hidboot_feed(&s,r2,8)==0&&ngot==2);
 assert(got[0].code==42&&got[0].value==1&&got[1].code==57&&got[1].value==1);
 uint8_t r3[8]={0,0,0x2c};                     /* up arrow and shift released */
 ngot=0;assert(hidboot_feed(&s,r3,8)==0&&ngot==2);
 assert(got[0].code==103&&got[0].value==0&&got[1].code==42&&got[1].value==0);
 ngot=0;assert(hidboot_feed(&s,r3,8)==0&&ngot==0);  /* same report: nothing */
 assert(hidboot_release_all(&s)==0&&ngot==1&&got[0].code==57&&got[0].value==0);
}
static void rollover(void) {
 struct hidboot s;hidboot_init(&s);ngot=0;
 uint8_t a[8]={0,0,0x28};hidboot_feed(&s,a,8);assert(ngot==1&&got[0].code==28);
 uint8_t e[8]={0,0,1,1,1,1,1,1};ngot=0;
 assert(hidboot_feed(&s,e,8)==0&&ngot==0);     /* ignored, Enter stays held */
 uint8_t z[8]={0};assert(hidboot_feed(&s,z,8)==0&&ngot==1&&got[0].code==28&&got[0].value==0);
 uint8_t lens[4]={0};assert(hidboot_feed(&s,lens,4)==-22&&hidboot_feed(&s,lens,9)==-22);
}
static void big_and_dups(void) {
 struct hidboot s;hidboot_init(&s);ngot=frames=0;
 uint8_t a[8]={0xff,0,4,5,6,7,8,9};            /* all modifiers + six letters: 14 */
 assert(hidboot_feed(&s,a,8)==0&&ngot==14&&frames==1);
 uint8_t b[8]={0,0,10,11,12,13,14,15};         /* 8 + 6 up, 6 down = 20: two frames */
 ngot=frames=0;assert(hidboot_feed(&s,b,8)==0&&ngot==20&&frames==2);
 uint8_t d[8]={0,0,4,4,0xe0,0,0,0};            /* duplicate and unmapped usages: one press */
 struct hidboot t;hidboot_init(&t);ngot=0;hidboot_feed(&t,d,8);assert(ngot==1&&got[0].code==30);
}
static void keymap(void) {
 assert(hidboot_linux_key(0x04)==30&&hidboot_linux_key(0x1d)==44&&hidboot_linux_key(0x1e)==2);
 assert(hidboot_linux_key(0x27)==11&&hidboot_linux_key(0x28)==28&&hidboot_linux_key(0x29)==1);
 assert(hidboot_linux_key(0x2c)==57&&hidboot_linux_key(0x3a)==59&&hidboot_linux_key(0x43)==68);
 assert(hidboot_linux_key(0x44)==87&&hidboot_linux_key(0x45)==88);
 assert(hidboot_linux_key(0x4f)==106&&hidboot_linux_key(0x50)==105&&hidboot_linux_key(0x51)==108&&hidboot_linux_key(0x52)==103);
 assert(hidboot_linux_key(0x58)==96&&hidboot_linux_key(0x59)==79&&hidboot_linux_key(0x62)==82&&hidboot_linux_key(0x63)==83);
 assert(hidboot_linux_key(0)==0&&hidboot_linux_key(1)==0&&hidboot_linux_key(0x64)==0&&hidboot_linux_key(0xff)==0);
 for(unsigned u=4;u<=0x63;u++) assert(hidboot_linux_key((uint8_t)u)!=0);
}
static void descriptors(void) {
 struct usbdesc_kbd k;
 uint8_t *b=copy(kb_first,sizeof kb_first,1);
 assert(usbdesc_find_boot_keyboard(b,sizeof kb_first,&k)==USBDESC_OK);
 assert(k.config_value==1&&k.interface_num==0&&k.endpoint==0x81&&k.interval==10&&k.max_packet==8);
 free(b);
 b=copy(kb_second,sizeof kb_second,1);
 assert(usbdesc_find_boot_keyboard(b,sizeof kb_second,&k)==USBDESC_OK);
 assert(k.interface_num==1&&k.endpoint==0x82&&k.interval==8);
 /* every truncation of a good descriptor is refused, and never read past len */
 for(unsigned n=0;n<sizeof kb_second;n++) {
  uint8_t *t=malloc(n?n:1);memcpy(t,b,n);
  assert(usbdesc_find_boot_keyboard(t,n,&k)==USBDESC_E_MALFORMED);free(t);
 }
 /* bLength 0 and 1 inside the walk; overrun; wrong first type; bad total */
 unsigned n=sizeof kb_second;uint8_t *m=copy(b,n,0);
 m[9]=0;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);
 m[9]=1;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);
 m[9]=250;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);
 free(m);m=copy(b,n,0);m[1]=1;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);
 m[1]=2;m[0]=8;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);
 m[0]=9;m[2]=8;m[3]=0;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);
 m[2]=0xff;m[3]=0xff;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);
 free(m);
 assert(usbdesc_find_boot_keyboard(0,9,&k)==USBDESC_E_MALFORMED&&usbdesc_find_boot_keyboard(b,n,0)==USBDESC_E_MALFORMED);
 /* a short endpoint descriptor (bLength 5) is malformed, not read as 7 bytes */
 m=copy(b,n,0);m[9+9+9]=5;assert(usbdesc_find_boot_keyboard(m,n,&k)==USBDESC_E_MALFORMED);free(m);
 /* well formed but no keyboard: the mouse and consumer interfaces only; and an OUT-only endpoint */
 static const uint8_t none[]={CFG(9+18+7),IF(0,1,2),HID,EP(0x81,4,10)};
 m=copy(none,sizeof none,1);assert(usbdesc_find_boot_keyboard(m,sizeof none,&k)==USBDESC_E_NOT_FOUND);free(m);
 static const uint8_t outonly[]={CFG(9+18+7),IF(0,1,1),HID,EP(0x01,8,10)};
 m=copy(outonly,sizeof outonly,1);assert(usbdesc_find_boot_keyboard(m,sizeof outonly,&k)==USBDESC_E_NOT_FOUND);free(m);
 free(b);
}
static void fuzz_return_codes(void) {
 struct usbdesc_kbd k;uint32_t x=12345;
 for(unsigned i=0;i<20000;i++) {
  uint8_t buf[64];unsigned n=(x=x*1664525u+1013904223u)>>26;
  for(unsigned j=0;j<n;j++) buf[j]=(uint8_t)((x=x*1664525u+1013904223u)>>24);
  if(n>=4&&(i&1)) {buf[0]=9;buf[1]=2;buf[2]=(uint8_t)n;buf[3]=0;}
  int r=usbdesc_find_boot_keyboard(buf,n,&k);
  assert(r==USBDESC_OK||r==USBDESC_E_MALFORMED||r==USBDESC_E_NOT_FOUND);
  struct hidboot s;hidboot_init(&s);struct input_change c[HIDBOOT_CHANGES_MAX];
  int d=hidboot_diff(&s,buf,n,c);assert(d>=0?(unsigned)d<=HIDBOOT_CHANGES_MAX:n!=8);
 }
}
int main(void) {down_up();rollover();big_and_dups();keymap();descriptors();fuzz_return_codes();return 0;}
