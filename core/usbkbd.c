/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usbkbd.h"
#include "hidboot.h"
#include "input.h"
void log_line(const char *s); /* kernel.h, without its hardware inlines */
static struct hidboot kb;static bool ready,attached;
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
static void hex4(char *o,uint16_t v) {for(int i=0;i<4;i++)o[i]="0123456789ABCDEF"[(v>>(12-4*i))&15];}
void usbkbd_init(void) {hidboot_init(&kb);ready=false;attached=false;}
void usbkbd_host_up(void) {log_line("USB host up");}
void usbkbd_attached(uint16_t vid,uint16_t pid) {
 char id[10];hex4(id,vid);id[4]=':';hex4(id+5,pid);id[9]=0;attached=true;say("device attached ",id);
}
void usbkbd_detached(void) {attached=false;log_line("device removed");}
bool usbkbd_hid_mounted(bool boot_keyboard) {
 if(!boot_keyboard) {log_line("HID interface skipped (not a boot keyboard)");return false;}
 if(ready) return false; /* one keyboard at a time */
 hidboot_init(&kb);input_add();ready=true;log_line("keyboard ready");return true;
}
void usbkbd_report(const uint8_t *rep,uint32_t len) {
 if(!ready) return;
 struct hidboot probe=kb;struct input_change ch[HIDBOOT_CHANGES_MAX];
 int n=hidboot_diff(&probe,rep,len,ch);
 for(int i=0;i<n;i++) if(ch[i].value==1) say("key ",code_name(ch[i].code));
 hidboot_feed(&kb,rep,len);
}
void usbkbd_hid_unmounted(void) {
 if(!ready) return;
 hidboot_release_all(&kb);input_remove();ready=false;log_line("keyboard removed");
}
bool usbkbd_ready(void) {return ready;}
const char *usbkbd_status(void) {return ready?"HOST: keyboard":attached?"HOST: device, no keyboard":"HOST: no device";}
