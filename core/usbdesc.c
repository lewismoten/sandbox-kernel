/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Walks a USB configuration descriptor (USB 2.0 spec chapter 9.6) for the HID boot keyboard.
 * Every step checks bLength against the bytes that remain, so damaged input is refused. */
#include "usbdesc.h"
#include <stddef.h>

#define DT_CONFIG 2u
#define DT_INTERFACE 4u
#define DT_ENDPOINT 5u

int usbdesc_find_boot_keyboard(const uint8_t *buf,uint32_t len,struct usbdesc_kbd *out) {
 if(!buf||!out||len<9u||buf[0]!=9u||buf[1]!=DT_CONFIG) return USBDESC_E_MALFORMED;
 uint32_t total=(uint32_t)buf[2]|((uint32_t)buf[3]<<8);
 if(total<9u||total>len) return USBDESC_E_MALFORMED;
 uint32_t pos=buf[0];
 int in_kbd=0,found=0;struct usbdesc_kbd res={0};                 /* the current interface is a boot keyboard, alternate 0 */
 uint8_t ifnum=0;
 while(pos<total) {
  if(total-pos<2u) return USBDESC_E_MALFORMED;
  uint32_t l=buf[pos];
  if(l<2u||l>total-pos) return USBDESC_E_MALFORMED;
  uint8_t type=buf[pos+1];
  if(type==DT_INTERFACE) {
   if(l<9u) return USBDESC_E_MALFORMED;
   ifnum=buf[pos+2];
   in_kbd=buf[pos+3]==0&&buf[pos+5]==3u&&buf[pos+6]==1u&&buf[pos+7]==1u;
  } else if(type==DT_ENDPOINT) {
   if(l<7u) return USBDESC_E_MALFORMED;
   uint16_t mps=(uint16_t)(buf[pos+4]|((buf[pos+5]&7u)<<8));   /* bits 10..0 */
   if(!found&&in_kbd&&(buf[pos+2]&0x80u)&&(buf[pos+3]&3u)==3u&&mps>=8u) {
    found=1;res.config_value=buf[5];res.interface_num=ifnum;res.endpoint=buf[pos+2];
    res.interval=buf[pos+6];res.max_packet=mps;   /* published only after the whole chain checks out */
   }
  }
  pos+=l;
 }
 if(!found) return USBDESC_E_NOT_FOUND;
 *out=res;
 return USBDESC_OK;
}
