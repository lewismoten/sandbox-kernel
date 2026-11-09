/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usbrole.h"
#include <assert.h>
int main(void) {
 struct usbrole s;usbrole_init(&s);
 assert(usbrole_id_grounded(0)&&!usbrole_id_grounded(1u<<16)&&usbrole_id_grounded(0xfffeffffu));
 assert(usbrole_step(&s,false,false,0)==USB_ROLE_DEVICE);      /* Mac cable, floating ID */
 assert(usbrole_step(&s,true,false,100)==USB_ROLE_DEVICE);     /* grounded, not yet stable */
 assert(usbrole_step(&s,true,false,599)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,600)==USB_ROLE_HOST_WANTED);
 assert(usbrole_step(&s,true,false,9000)==USB_ROLE_HOST_WANTED);
 assert(usbrole_step(&s,false,false,9010)==USB_ROLE_DEVICE);   /* adapter unplugged */
 assert(usbrole_step(&s,true,false,9020)==USB_ROLE_DEVICE);    /* replug restarts the debounce */
 assert(usbrole_step(&s,true,false,9519)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,9520)==USB_ROLE_HOST_WANTED);
 assert(usbrole_step(&s,true,true,9521)==USB_ROLE_DEVICE);     /* Mac link wins */
 assert(usbrole_step(&s,true,false,9522)==USB_ROLE_DEVICE);    /* and the debounce restarts */
 assert(usbrole_step(&s,true,false,9000+2000)==USB_ROLE_HOST_WANTED);
 usbrole_init(&s);                                             /* a glitch shorter than the window */
 usbrole_step(&s,true,false,0);usbrole_step(&s,false,false,300);
 assert(usbrole_step(&s,true,false,400)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,899)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,900)==USB_ROLE_HOST_WANTED);
 return 0;
}
