/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usbrole.h"
void usbrole_init(struct usbrole *s) {s->role=USB_ROLE_DEVICE;s->cand=false;s->since=0;}
enum usb_role usbrole_step(struct usbrole *s,bool id_grounded,bool enumerated,uint32_t now_us) {
 bool want=id_grounded&&!enumerated;
 if(!want) {s->cand=false;s->role=USB_ROLE_DEVICE;return s->role;}
 if(!s->cand) {s->cand=true;s->since=now_us;}
 if(now_us-s->since>=USBROLE_HOST_STABLE_US) s->role=USB_ROLE_HOST_WANTED;
 return s->role;
}
