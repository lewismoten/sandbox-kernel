/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usbkbd.h"
#include "hidboot.h"
#include "input.h"
#include "usblog.h"
#include <string.h>
void log_line(const char *s); /* kernel.h, without its hardware inlines */
/* Up to USBKBD_MAX boot keyboards at once (a hub may hold two plus a receiver); the input queue sees one device. */
/* Each keyboard keeps its own last report (kb) and held keys; what the OS sees is the union (sent). */
static struct {struct hidboot kb;bool used,seen,idrep,shortnote;uint8_t daddr,idx;uint8_t held[16];} slot[USBKBD_MAX];static uint8_t sent[16];static unsigned nkb,ndev;
static const char *const special[]={"Enter","Esc","Backspace","Tab","Space","-","=","[","]","\\","#",";","'","`",",",".","/","CapsLock"};
const char *usbkbd_usage_name(uint8_t u) {
 static char one[2],f[4];
 if(u>=0x04&&u<=0x1d) {one[0]=(char)('A'+u-4);one[1]=0;return one;}
 if(u>=0x1e&&u<=0x27) {one[0]=(char)(u==0x27?'0':'1'+u-0x1e);one[1]=0;return one;}
 if(u>=0x28&&u<=0x39) return special[u-0x28];
 if(u>=0x3a&&u<=0x45) {unsigned n=u-0x3au+1;f[0]='F';if(n>=10){f[1]='1';f[2]=(char)('0'+n-10);f[3]=0;}else{f[1]=(char)('0'+n);f[2]=0;}return f;}
 switch(u) {case 0x4f:return "Right";case 0x50:return "Left";case 0x51:return "Down";case 0x52:return "Up";
  case 0x49:return "Insert";case 0x4a:return "Home";case 0x4b:return "PageUp";case 0x4c:return "Delete";case 0x4d:return "End";case 0x4e:return "PageDown";
  case 0x53:return "NumLock";case 0x58:return "KeypadEnter";default:break;}
 if(u>=0x54&&u<=0x63) return "Keypad";
 return 0;
}
static const char *code_name(uint16_t code) {
 static const struct {uint16_t c;const char *n;} mod[]={{29,"LCtrl"},{42,"LShift"},{56,"LAlt"},{125,"LGui"},{97,"RCtrl"},{54,"RShift"},{100,"RAlt"},{126,"RGui"}};
 for(unsigned i=0;i<8;i++) if(mod[i].c==code) return mod[i].n;
 for(unsigned u=4;u<256;u++) if(hidboot_linux_key((uint8_t)u)==code) {const char *n=usbkbd_usage_name((uint8_t)u);if(n) return n;}
 return "?";
}
static void say(const char *a,const char *b) {
 char t[48];unsigned n=0;while(*a&&n<30)t[n++]=*a++;while(b&&*b&&n<46)t[n++]=*b++;t[n]=0;log_line(t);
}
static void note(uint8_t daddr,uint8_t idx,const char *t) { /* "D2 hid1 text": idx is TinyUSB's HID index (the driver logs which interface it is) */
 char x[USBLOG_LINE-12]="hid0 ";x[3]=(char)('0'+idx%10);unsigned n=5;while(*t&&n<sizeof x-1)x[n++]=*t++;x[n]=0;
 char l[USBLOG_LINE];usblog_fmt_note(l,daddr,0xFF,x);usblog_add(l);}
static void hex4(char *o,uint16_t v) {for(int i=0;i<4;i++)o[i]="0123456789ABCDEF"[(v>>(12-4*i))&15];}
void usbkbd_init(void) {usblog_clear();for(unsigned i=0;i<USBKBD_MAX;i++)slot[i].used=false;for(unsigned i=0;i<16;i++)sent[i]=0;nkb=0;ndev=0;}
void usbkbd_host_up(void) {log_line("USB host up");}
void usbkbd_attached(uint16_t vid,uint16_t pid) {
 char id[10];hex4(id,vid);id[4]=':';hex4(id+5,pid);id[9]=0;ndev++;say("device attached ",id);
}
void usbkbd_detached(void) {if(ndev)ndev--;log_line("device removed");}
static int find(uint8_t daddr,uint8_t idx) {for(unsigned i=0;i<USBKBD_MAX;i++)if(slot[i].used&&slot[i].daddr==daddr&&slot[i].idx==idx)return (int)i;return -1;}
bool usbkbd_hid_mounted(uint8_t daddr,uint8_t idx,bool boot_keyboard) {
 if(!boot_keyboard) {log_line("HID interface skipped (not a boot keyboard)");note(daddr,idx,"HID skipped: not a boot keyboard");return false;}
 if(find(daddr,idx)>=0) {note(daddr,idx,"HID skipped: already mounted");return false;}
 for(unsigned i=0;i<USBKBD_MAX;i++) if(!slot[i].used) {
  hidboot_init(&slot[i].kb);memset(slot[i].held,0,16);slot[i].seen=slot[i].idrep=slot[i].shortnote=false;slot[i].used=true;slot[i].daddr=daddr;slot[i].idx=idx;
  if(!nkb++) input_add();
  log_line("keyboard ready");note(daddr,idx,"keyboard ACCEPTED");return true;
 }
 log_line("keyboard skipped (too many)");note(daddr,idx,"keyboard skipped: HID slots full");return false;
}
/* Send what changed in the union of every keyboard's held keys: releases first, modifiers before keys on a press. */
static void sync_union(void) {
 uint8_t u[16]={0};for(unsigned i=0;i<USBKBD_MAX;i++) if(slot[i].used) for(unsigned j=0;j<16;j++) u[j]|=slot[i].held[j];
 static const uint16_t mods[8]={29,42,56,125,97,54,100,126};
 struct input_change ch[128];unsigned n=0;
 for(int pass=0;pass<4;pass++) for(unsigned k=1;k<128;k++) {
  unsigned was=(sent[k>>3]>>(k&7))&1u,now=(u[k>>3]>>(k&7))&1u;int m=0;for(unsigned q=0;q<8;q++) if(mods[q]==k) m=1;
  if(was==now||(pass==0&&(!was||m))||(pass==1&&(!was||!m))||(pass==2&&(was||!m))||(pass==3&&(was||m))) continue;
  ch[n].type=EV_KEY;ch[n].code=(uint16_t)k;ch[n].value=(int)now;n++;
 }
 memcpy(sent,u,16);
 for(unsigned i=0;i<n;i+=INPUT_FRAME_MAX) {unsigned c=n-i>INPUT_FRAME_MAX?INPUT_FRAME_MAX:n-i;(void)input_frame(ch+i,c);}
}
static void note_len(uint8_t daddr,uint8_t idx,const char *t,uint32_t len) {
 char x[24];unsigned n=0;while(*t&&n<14)x[n++]=*t++;char d[11];unsigned k=0;do d[k++]=(char)('0'+len%10);while((len/=10)&&k<10);while(k)x[n++]=d[--k];x[n]=0;note(daddr,idx,x);
}
bool usbkbd_report(uint8_t daddr,uint8_t idx,const uint8_t *rep,uint32_t len) {
 int i=find(daddr,idx);if(i<0) return false;
 if(!slot[i].seen) {slot[i].seen=true;slot[i].idrep=len==HIDBOOT_REPORT+1;note_len(daddr,idx,"first report len ",len);
  if(slot[i].idrep) note(daddr,idx,"report ID byte stripped");}
 if(len<HIDBOOT_REPORT) {if(!slot[i].shortnote) {slot[i].shortnote=true;note_len(daddr,idx,"short report ignored len ",len);}return true;}
 if(slot[i].idrep&&len>HIDBOOT_REPORT) {rep++;len--;}   /* leading report ID */
 struct input_change ch[HIDBOOT_CHANGES_MAX];
 int n=hidboot_diff(&slot[i].kb,rep,HIDBOOT_REPORT,ch);
 for(int k=0;k<n;k++) if(ch[k].value==1) say("key ",code_name(ch[k].code));
 hidboot_held(slot[i].kb.prev,slot[i].held);sync_union();
 return true;
}
static void drop_slot(unsigned i,const char *why) {
 memset(slot[i].held,0,16);slot[i].used=false;sync_union();
 if(!--nkb) input_remove();
 log_line(why);note(slot[i].daddr,slot[i].idx,why);
}
void usbkbd_hid_unmounted(uint8_t daddr,uint8_t idx) {int i=find(daddr,idx);if(i>=0) drop_slot((unsigned)i,"keyboard removed");}
/* The device is gone, whichever interface: every keyboard on that device address. */
void usbkbd_device_gone(uint8_t daddr) {for(unsigned i=0;i<USBKBD_MAX;i++) if(slot[i].used&&slot[i].daddr==daddr) drop_slot(i,"keyboard removed");}
void usbkbd_poll_failed(uint8_t daddr,uint8_t idx) {int i=find(daddr,idx);if(i>=0) drop_slot((unsigned)i,"keyboard poll failed");}
bool usbkbd_ready(void) {return nkb>0;}
unsigned usbkbd_count(void) {return nkb;}
const char *usbkbd_status(void) {return nkb?"HOST: keyboard":ndev?"HOST: device, no keyboard":"HOST: no device";}
