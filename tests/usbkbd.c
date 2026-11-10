/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* The host-role callback glue (core/usbkbd.c) with fakes for the log, the input queue and a
 * scripted run of what TinyUSB's callbacks would do for a Rii-style boot keyboard. */
#include "usbkbd.h"
#include "input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char lines[64][64];static unsigned nlines,adds,removes;
static struct input_change got[64];static unsigned ngot;
void log_line(const char *s) {assert(nlines<64&&strlen(s)<64);strcpy(lines[nlines++],s);}
void input_add(void) {adds++;}
void input_remove(void) {removes++;}
int input_frame(const struct input_change *c,unsigned n) {memcpy(got+ngot,c,n*sizeof *c);ngot+=n;return 0;}
static int has(const char *s) {for(unsigned i=0;i<nlines;i++) if(!strcmp(lines[i],s)) return 1;return 0;}
static void reset(void) {nlines=adds=removes=ngot=0;usbkbd_init();}

int main(void) {
 reset();
 assert(!strcmp(usbkbd_status(),"HOST: no device"));
 usbkbd_host_up();usbkbd_attached(0x1a2c,0x0c);
 assert(!strcmp(lines[0],"USB host up")&&!strcmp(lines[1],"device attached 1A2C:000C"));
 assert(!strcmp(usbkbd_status(),"HOST: device, no keyboard"));
 /* a mouse or consumer-control interface is skipped and never polled */
 assert(!usbkbd_hid_mounted(1,0,false)&&!usbkbd_ready()&&adds==0);
 /* reports before the keyboard is ready are ignored */
 uint8_t r0[8]={0,0,0x04};usbkbd_report(1,1,r0,8);assert(ngot==0);
 assert(usbkbd_hid_mounted(1,1,true)&&usbkbd_ready()&&adds==1&&has("keyboard ready"));
 assert(!usbkbd_hid_mounted(1,1,true)); /* second keyboard interface: not polled */
 assert(!strcmp(usbkbd_status(),"HOST: keyboard"));
 /* Up arrow, then Space and Enter while Up is held */
 nlines=0;uint8_t up[8]={0,0,0x52};usbkbd_report(1,1,up,8);
 assert(ngot==1&&got[0].code==103&&got[0].value==1&&nlines==1&&!strcmp(lines[0],"key Up"));
 nlines=0;ngot=0;uint8_t sp[8]={0,0,0x52,0x2c,0x28};usbkbd_report(1,1,sp,8);
 assert(ngot==2&&nlines==2&&!strcmp(lines[0],"key Space")&&!strcmp(lines[1],"key Enter"));
 /* a release is a change but not a "key" line */
 nlines=0;ngot=0;uint8_t none[8]={0};usbkbd_report(1,1,none,8);
 assert(ngot==3&&nlines==0);
 /* modifier and letter names */
 nlines=0;uint8_t sh[8]={2,0,0x04};usbkbd_report(1,1,sh,8);
 assert(nlines==2&&!strcmp(lines[0],"key LShift")&&!strcmp(lines[1],"key A"));
 /* hold a key, then unplug: the release is sent and the device removed, no stuck key */
 ngot=0;usbkbd_device_gone();
 assert(ngot==2&&got[0].value==0&&got[1].value==0&&removes==1&&!usbkbd_ready()&&has("keyboard removed"));
 usbkbd_device_gone();assert(removes==1);   /* the device-gone callback after it is harmless */
 usbkbd_detached();assert(has("device removed")&&!strcmp(usbkbd_status(),"HOST: no device"));
 /* replug works as a new device */
 usbkbd_attached(0x1a2c,0x0c);assert(usbkbd_hid_mounted(1,1,true)&&adds==2);
 /* a short or rollover report changes nothing and logs nothing */
 nlines=0;ngot=0;uint8_t bad[3]={0,0,4};usbkbd_report(1,1,bad,3);uint8_t ro[8]={0,0,1,1,1,1,1,1};usbkbd_report(1,1,ro,8);
 assert(ngot==0&&nlines==0);
 /* every usage has a name or none; names are short */
 for(unsigned u=0;u<256;u++) {const char *n=usbkbd_usage_name((uint8_t)u);assert(!n||(strlen(n)>0&&strlen(n)<12));}
 assert(!strcmp(usbkbd_usage_name(0x52),"Up")&&!strcmp(usbkbd_usage_name(0x27),"0")&&!strcmp(usbkbd_usage_name(0x45),"F12")&&!strcmp(usbkbd_usage_name(0x3a),"F1"));
 puts("usbkbd ok");return 0;
}
