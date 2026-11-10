/* SPDX-License-Identifier: MIT; TinyUSB host-role glue (build with USB_ROLE=host). Replaces
 * drivers/usb.c: the single OTG port is a host, so there is no Mac link, no CDC and no
 * protocol traffic. A HID boot keyboard feeds the input queue through core/usbkbd.c. */
#include "kernel.h"
#include "tusb.h"
#include "usbkbd.h"
static bool initialized;
static const char *init_fail="unavailable";
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
