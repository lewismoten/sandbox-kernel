/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "bootinfo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 char b[BOOTINFO_REC_MAX];uint32_t n;
 assert(bootinfo_format(b,30,BD_KEYBOARD,2)==15&&!strcmp(b,"n30,keyboard,k2"));
 assert(!strcmp(b,"n30,keyboard,k2")&&bootinfo_parse_count(b,15,&n)&&n==30);
 bootinfo_format(b,0,BD_NO_ADAPTER,0);assert(!strcmp(b,"n0,no-adapter,k0"));
 bootinfo_format(b,4294967295u,BD_INSTALL_KBD,99);assert(!strcmp(b,"n4294967295,install-kbd,k9")&&strlen(b)<BOOTINFO_REC_MAX);
 assert(bootinfo_parse_count("n7,link,k0\n",11,&n)&&n==7);
 assert(!bootinfo_parse_count("x7",2,&n)&&!bootinfo_parse_count("n,",2,&n)&&!bootinfo_parse_count("n99999999999",12,&n)&&!bootinfo_parse_count("",0,&n));
 assert(!strcmp(bootinfo_status(BD_KEYBOARD),"KEYBOARD MODE")&&!strcmp(bootinfo_status(BD_NO_ADAPTER),"NO OTG ADAPTER")&&!strcmp(bootinfo_status(BD_INSTALL_KBD),"OTG: INSTALL KBD"));
 assert(!bootinfo_status(BD_LINK)&&!bootinfo_status(BD_NONE));
 for(int d=0;d<=BD_KEYBOARD;d++) {const char *s=bootinfo_status((enum boot_decision)d);assert(!s||strlen(s)<=16);}
 char nm[BOOTINFO_NAME_MAX+1];
 assert(bootinfo_parse_name("PI-2\n",5,nm)&&!strcmp(nm,"PI-2"));
 assert(bootinfo_parse_name("PI-2\r\nextra",11,nm)&&!strcmp(nm,"PI-2"));
 assert(bootinfo_parse_name("ABCDEFGH",8,nm)&&!strcmp(nm,"ABCDEFGH"));
 assert(!bootinfo_parse_name("ABCDEFGHI",9,nm)&&!nm[0]);           /* 9 chars */
 assert(!bootinfo_parse_name("pi-2",4,nm)&&!bootinfo_parse_name("PI 2",4,nm)&&!bootinfo_parse_name("\nPI",3,nm)&&!bootinfo_parse_name("",0,nm));
 bootinfo_set_prev("n29,keyboard,k2\nrest",20);assert(!strcmp(bootinfo_prev(),"n29,keyboard,k2"));
 bootinfo_set_name("PI-2");assert(!strcmp(bootinfo_name(),"PI-2"));
 puts("bootinfo ok");return 0;
}
