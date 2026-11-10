/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* Review findings R2-R4 of the host-role keyboard, with the real usbkbd, hidboot and input code. */
#include "kernel.h"
#include "clock.h"
#include "usbkbd.h"
#include "input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint64_t clock_us(void) {return 1;}
void log_line(const char *s) {(void)s;}
#define KEY_A 30
static int held(unsigned code) {uint64_t k[INPUT_KEYS/64]={0};int64_t a[INPUT_AXES];input_open(input_instance());assert(!input_state(input_instance(),k,a));return (int)((k[code/64]>>(code%64))&1);}
static const uint8_t A[8]={0,0,4},NONE[8]={0};
int main(void) {
 /* R3: input_init runs first (main.c), then a keyboard mounts and a held key arrives. */
 input_init();usbkbd_init();
 assert(usbkbd_hid_mounted(1,0,true)&&usbkbd_report(1,0,A,8)&&held(KEY_A));
 /* R2: mouse first, keyboard second, consumer control third, a second keyboard fourth. */
 input_init();usbkbd_init();
 assert(!usbkbd_hid_mounted(1,0,false));                 /* mouse */
 assert(usbkbd_hid_mounted(1,1,true));                   /* keyboard, not the first interface */
 assert(!usbkbd_hid_mounted(1,2,false));                 /* consumer */
 assert(usbkbd_hid_mounted(1,3,true)&&usbkbd_count()==2); /* a second keyboard is polled too (one queue device) */
 assert(!usbkbd_report(1,0,A,8)&&!held(KEY_A));           /* a mouse report is never read as keys */
 assert(usbkbd_report(1,1,A,8)&&held(KEY_A));
 usbkbd_hid_unmounted(1,0);usbkbd_hid_unmounted(1,2);usbkbd_hid_unmounted(1,3); /* others leaving keep the keyboard */
 assert(usbkbd_ready()&&input_present()&&held(KEY_A));
 usbkbd_hid_unmounted(1,1);assert(!usbkbd_ready()&&!input_present());
 /* keyboard listed before the mouse works the same */
 usbkbd_init();input_init();
 assert(usbkbd_hid_mounted(1,0,true)&&!usbkbd_hid_mounted(1,1,false)&&usbkbd_report(1,0,A,8)&&held(KEY_A));
 /* R4: a refused re-arm after a held key removes the keyboard: key released, status truthful. */
 usbkbd_attached(1,2);
 usbkbd_poll_failed(1,0);
 assert(!usbkbd_ready()&&!input_present()&&!strcmp(usbkbd_status(),"HOST: device, no keyboard"));
 assert(!usbkbd_report(1,0,NONE,8));                      /* nothing is read after the removal */
 /* a refused first poll removes it too, and a replug starts clean */
 input_init();usbkbd_init();
 assert(usbkbd_hid_mounted(1,0,true));usbkbd_poll_failed(1,0);assert(!usbkbd_ready()&&!input_present());
 input_add();assert(usbkbd_hid_mounted(1,0,true)&&usbkbd_report(1,0,A,8)&&held(KEY_A));
 usbkbd_device_gone(1);assert(!usbkbd_ready());
 puts("usbhost regress ok");return 0;
}
