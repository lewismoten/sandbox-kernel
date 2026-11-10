/* SPDX-License-Identifier: MIT; TinyUSB host-role glue (build with USB_ROLE=host). Replaces
 * drivers/usb.c: the single OTG port is a host, so there is no Mac link, no CDC and no
 * protocol traffic. A HID boot keyboard feeds the input queue through core/usbkbd.c. */
#include "kernel.h"
#include "tusb.h"
#include "usbkbd.h"
static bool initialized;
static const char *init_fail="unavailable";
/* TinyUSB calls this before SET_CONFIGURATION and before any class opens endpoints.
   Reject the entire configuration if any HID IN endpoint exceeds its receive buffer,
   including interfaces our keyboard mount callback would otherwise skip. */
bool tuh_enum_descriptor_configuration_cb(uint8_t daddr,uint8_t cfg_index,const tusb_desc_configuration_t *config) {
 (void)daddr;(void)cfg_index;
 const uint8_t *p=(const uint8_t *)config;
 uint16_t total=(uint16_t)(p[2]|((uint16_t)p[3]<<8));
 if(total<sizeof(*config)||total>CFG_TUH_ENUMERATION_BUFSIZE) return false;
 bool hid=false;
 for(uint16_t off=0;off<total;) {
  if(total-off<2||p[off]<2||p[off]>total-off) return false;
  uint8_t len=p[off],type=p[off+1];
  if(type==TUSB_DESC_INTERFACE) {
   if(len<sizeof(tusb_desc_interface_t)) return false;
   hid=p[off+5]==TUSB_CLASS_HID;
  } else if(type==TUSB_DESC_ENDPOINT&&hid) {
   if(len<sizeof(tusb_desc_endpoint_t)) return false;
   uint16_t size=(uint16_t)(p[off+4]|((uint16_t)p[off+5]<<8))&0x7ffu;
   if((p[off+2]&0x80u)&&size>CFG_TUH_HID_EPIN_BUFSIZE) {
    log_line("USB host: HID IN packet exceeds receive buffer");
    return false; /* one log per rejected configuration, no mount/re-arm loop */
   }
  }
  off+=len;
 }
 return true;
}
void usb_irq(void) {if(initialized)tuh_int_handler(0,true);}
void tuh_mount_cb(uint8_t daddr) {uint16_t vid=0,pid=0;tuh_vid_pid_get(daddr,&vid,&pid);usbkbd_attached(vid,pid);}
void tuh_umount_cb(uint8_t daddr) {(void)daddr;usbkbd_device_gone();usbkbd_detached();}
/* R4: arm the interrupt endpoint, retrying a refused submit a few times; if it stays refused the
   keyboard is removed (keys released, status truthful) rather than left ready with no poll. */
static void arm(uint8_t daddr,uint8_t idx) {
 for(unsigned n=0;n<3;n++) if(tuh_hid_receive_report(daddr,idx)) return;
 usbkbd_poll_failed();
}
void tuh_hid_mount_cb(uint8_t daddr,uint8_t idx,const uint8_t *desc,uint16_t len) {
 (void)desc;(void)len;
 /* TinyUSB's default protocol is boot, set before this callback for boot-subclass interfaces. */
 bool boot_kbd=tuh_hid_interface_protocol(daddr,idx)==HID_ITF_PROTOCOL_KEYBOARD&&tuh_hid_get_protocol(daddr,idx)==HID_PROTOCOL_BOOT;
 if(usbkbd_hid_mounted(daddr,idx,boot_kbd)) arm(daddr,idx);
}
void tuh_hid_umount_cb(uint8_t daddr,uint8_t idx) {usbkbd_hid_unmounted(daddr,idx);}
void tuh_hid_report_received_cb(uint8_t daddr,uint8_t idx,const uint8_t *report,uint16_t len) {
 if(usbkbd_report(daddr,idx,report,len)) arm(daddr,idx);
}
const char *usb_state(void) {return initialized?usbkbd_status():init_fail;}
void usb_init(void) {
 uint32_t p[2]={3,3};
 if(!property(0x28001,p,2)) {init_fail="USB POWER";error("USB POWER");log_line(property_fail);return;}
 usbkbd_init();
 initialized=true;
 if(!tusb_init()) {initialized=false;init_fail="TUSB HOST INIT";error("USB host init failed");return;}
 usbkbd_host_up();
}
void usb_reply(const char *s) {(void)s;}
bool usb_reply_pending(void) {return false;}
void usb_log(const char *s) {(void)s;}
void usb_task(void) {if(initialized)tuh_task();}
void service_io(void) {usb_task();protocol_task();}
uint32_t tusb_time_millis_api(void) {return micros()/1000;}
bool usb_healthy(void) {return initialized;}
