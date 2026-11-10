/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usbrole.h"
#include <assert.h>
#include <string.h>
int main(void) {
 struct usbrole s;usbrole_init(&s);
 assert(usbrole_id_grounded(0)&&!usbrole_id_grounded(1u<<16)&&usbrole_id_grounded(0xfffeffffu));
 assert(usbrole_step(&s,false,false,0u)==USB_ROLE_DEVICE);      /* Mac cable, floating ID */
 assert(usbrole_step(&s,true,false,100000u)==USB_ROLE_DEVICE);     /* grounded, not yet stable */
 assert(usbrole_step(&s,true,false,599000u)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,600000u)==USB_ROLE_HOST_WANTED);
 assert(usbrole_step(&s,true,false,9000000u)==USB_ROLE_HOST_WANTED);
 assert(usbrole_step(&s,false,false,9010000u)==USB_ROLE_DEVICE);   /* adapter unplugged */
 assert(usbrole_step(&s,true,false,9020000u)==USB_ROLE_DEVICE);    /* replug restarts the debounce */
 assert(usbrole_step(&s,true,false,9519000u)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,9520000u)==USB_ROLE_HOST_WANTED);
 assert(usbrole_step(&s,true,true,9521000u)==USB_ROLE_DEVICE);     /* Mac link wins */
 assert(usbrole_step(&s,true,false,9522000u)==USB_ROLE_DEVICE);    /* and the debounce restarts */
 assert(usbrole_step(&s,true,false,(9000+2000)*1000u)==USB_ROLE_HOST_WANTED);
 usbrole_init(&s);                                             /* a glitch shorter than the window */
 usbrole_step(&s,true,false,0u);usbrole_step(&s,false,false,300000u);
 assert(usbrole_step(&s,true,false,400000u)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,899000u)==USB_ROLE_DEVICE);
 assert(usbrole_step(&s,true,false,900000u)==USB_ROLE_HOST_WANTED);
 /* R2: micros() wraps at 2^32 us; deltas must stay right across it. */
 usbrole_init(&s);
 {uint32_t t0=0xffffffffu-250000u;                 /* 250 ms before the wrap */
  assert(usbrole_step(&s,true,false,t0)==USB_ROLE_DEVICE);
  assert(usbrole_step(&s,true,false,t0+499999u)==USB_ROLE_DEVICE);   /* wraps to 249998 */
  assert(usbrole_step(&s,true,false,t0+500000u)==USB_ROLE_HOST_WANTED); /* exactly 500 ms, after the wrap */
  usbrole_init(&s);
  assert(usbrole_step(&s,true,false,0xfffffff0u)==USB_ROLE_DEVICE);   /* start 16 us before the wrap */
  assert(usbrole_step(&s,true,false,0x0007a10fu)==USB_ROLE_DEVICE);   /* 499,999 us later (wrapped) */
  assert(usbrole_step(&s,true,false,0x0007a110u)==USB_ROLE_HOST_WANTED);}
 {                                                 /* start 65,536 us before the wrap (the reviewer saw the old code fail near 68,296 us) */
  usbrole_init(&s);
  assert(usbrole_step(&s,true,false,0xffff0000u)==USB_ROLE_DEVICE);
  assert(usbrole_step(&s,true,false,0xffff0000u+499999u)==USB_ROLE_DEVICE);
  assert(usbrole_step(&s,true,false,0xffff0000u+500000u)==USB_ROLE_HOST_WANTED);}
 /* R1: the ID window clears both force bits, never sets host, and is refused if the port is powered. */
 assert(usbrole_window_gusbcfg(0xffffffffu)==(0xffffffffu&~((1u<<29)|(1u<<30))));
 assert((usbrole_window_gusbcfg(0x60001234u)&((1u<<29)|(1u<<30)))==0);
 assert(usbrole_window_gusbcfg(0x00001234u)==0x00001234u);          /* touches only the force bits */
 assert(usbrole_device_gusbcfg(0x20000000u)==0x40000000u);          /* host force cleared, device forced */
 assert(!(usbrole_device_gusbcfg(0xffffffffu)&(1u<<29))&&(usbrole_device_gusbcfg(0)&(1u<<30)));
 assert(usbrole_window_may_open(0)&&usbrole_window_may_open(0xffffefffu));
 assert(!usbrole_window_may_open(1u<<12));                          /* port already powered: skip the window */
 assert(strlen(USBROLE_MISSING_COMPANION)<=16);
 assert(!strcmp(USBROLE_MISSING_COMPANION,"OTG: INSTALL KBD"));
 usbrole_init(&s);
 assert(usbrole_companion_action(usbrole_step(&s,true,false,0),true,true)==USB_COMPANION_NONE);
 assert(usbrole_companion_action(usbrole_step(&s,true,false,499999),true,true)==USB_COMPANION_NONE);
 assert(usbrole_companion_action(usbrole_step(&s,true,false,500000),true,true)==USB_COMPANION_HANDOFF);
 assert(usbrole_companion_action(s.role,false,true)==USB_COMPANION_MISSING);
 assert(usbrole_companion_action(s.role,true,false)==USB_COMPANION_NONE); /* never from a trial */
 assert(usbrole_companion_action(usbrole_step(&s,true,true,500001),true,true)==USB_COMPANION_NONE);
 assert(usbrole_companion_action(usbrole_step(&s,false,false,500002),true,true)==USB_COMPANION_NONE);
 return 0;
}
