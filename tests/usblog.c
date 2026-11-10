/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usblog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 char l[USBLOG_LINE];uint32_t n;
 usblog_fmt_device(l,3,1,2,"FS",true,0x1997,0x2433,0,0,0,32);
 assert(!strcmp(l,"D3 hub1 port2 FS split 1997:2433 class 0/0/0 ep0 32"));
 usblog_fmt_device(l,1,0,1,"HS",false,0x05E3,0x0608,9,0,1,64);
 assert(!strcmp(l,"D1 otg-port HS 05E3:0608 class 9/0/1 ep0 64"));
 usblog_fmt_interface(l,3,1,3,1,2,1);assert(!strcmp(l,"D3 I1 class 3/1/2 eps 1"));
 usblog_fmt_endpoint(l,3,0,0x81,3,8,10);assert(!strcmp(l,"D3 I0 ep 0x81 int mps 8 every 10"));
 usblog_fmt_note(l,3,0xFF,"mounted");assert(!strcmp(l,"D3 mounted"));
 usblog_fmt_note(l,3,2,"x");assert(!strcmp(l,"D3 I2 x"));
 usblog_fmt_control(l,1,0x80,6,0x100,0,18,1,20,"failed");
 assert(!strcmp(l,"D1 CTRL 80/06 v=0100 i=0000 len=18 failed retry=1 wait=20ms"));
 usblog_fmt_control(l,1,0x80,6,0x100,0,18,2,0,"recovered");
 assert(strstr(l,"recovered retry=2"));
 usblog_fmt_port_retry(l,5,2,2,"exhausted");assert(!strcmp(l,"hub5 port2 enum exhausted 2/2"));
 usblog_fmt_poll_retry(l,5,1,20,"refused");assert(!strcmp(l,"hub5 poll refused retry=1 wait=20ms"));
 usblog_fmt_poll_retry(l,5,3,0,"exhausted: reset");assert(!strcmp(l,"hub5 poll exhausted: reset retry=3"));
 usblog_fmt_poll_retry(l,5,2,0,"recovered");assert(!strcmp(l,"hub5 poll recovered retry=2"));
 usblog_fmt_port_retry(l,0,0,1,"reset retry");assert(!strcmp(l,"hub0 port0 enum reset retry 1/2"));
 usblog_clear();assert(usblog_version()>0);uint32_t v=usblog_version();
 usblog_add("one");usblog_add("two\nthree");assert(usblog_version()!=v);
 assert(!strcmp(usblog_text(&n),"one\ntwo three\n")&&n==14);
 usblog_clear();assert(!strcmp(usblog_text(&n),"")&&n==0);
 char big[USBLOG_LINE+40];memset(big,'x',sizeof big-1);big[sizeof big-1]=0;
 for(int i=0;i<100;i++) usblog_add(big);
 usblog_text(&n);assert(n<=USBLOG_MAX);assert(strstr(usblog_text(0),"log full\n"));
 uint32_t before=usblog_version();usblog_add("late");assert(usblog_version()==before); /* bounded: nothing more once full */
 puts("usblog ok");return 0;
}
