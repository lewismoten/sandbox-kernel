/* SPDX-License-Identifier: MIT; TinyUSB host-role glue (build with USB_ROLE=host). Replaces
 * drivers/usb.c: the single OTG port is a host, so there is no Mac link, no CDC and no
 * protocol traffic. A HID boot keyboard feeds the input queue through core/usbkbd.c. */
#include "kernel.h"
#include "clock.h"
#include "os.h"
#include "tusb.h"
#include "usbkbd.h"
#include "usblog.h"
#include "usbhost_hooks.h"
#include "usbhost_work.h"
#include "usb_wait.h"
#include "host/hcd.h"
static bool initialized;
static const char *init_fail="unavailable";
/* USB.TXT (core/usblog.c): each device as TinyUSB reads it, then what the kernel did with it. */
/* Every USB.TXT line from this driver starts with the milliseconds since boot ("12345 "), so a stall
 * during enumeration can be placed on the timeline (the log had no times before). */
static void now_us64(uint32_t *hi,uint32_t *lo) {
#ifdef HOST_TEST
 *hi=0;*lo=micros();
#else
 /* The system timer's high word (CHI) extends micros() (CLO) past its 71.6 minute wrap; reread if CHI changed between the reads. */
 uint32_t h,l;do{h=REG(PERIPH+0x3008);l=REG(PERIPH+0x3004);}while(h!=REG(PERIPH+0x3008));
 *hi=h;*lo=l;
#endif
}
static void lg(const char *l) {
 char t[USBLOG_LINE];unsigned a;uint32_t hi,lo;
 now_us64(&hi,&lo);a=usblog_stamp(t,hi,lo);
 for(;*l&&a<USBLOG_LINE-1;l++) t[a++]=*l;
 t[a]=0;usblog_add(t);
}
static const char *speed_name(uint8_t s) {return s==TUSB_SPEED_HIGH?"HS":s==TUSB_SPEED_LOW?"LS":s==TUSB_SPEED_FULL?"FS":"??";}
static struct {uint8_t d;bool mounted,warned;uint32_t t;} dv[8];
static volatile uint32_t ev_attach,ev_remove;
void tuh_event_hook_cb(uint8_t rhport,uint32_t eventid,bool in_isr) {
 (void)rhport;(void)in_isr;
 if(eventid==HCD_EVENT_DEVICE_ATTACH) ev_attach++; else if(eventid==HCD_EVENT_DEVICE_REMOVE) ev_remove++;
}
static void dv_seen(uint8_t d) {
 for(unsigned i=0;i<8;i++) if(dv[i].d==d) {dv[i].mounted=false;dv[i].warned=false;dv[i].t=micros();return;}
 for(unsigned i=0;i<8;i++) if(!dv[i].d) {dv[i].d=d;dv[i].mounted=false;dv[i].warned=false;dv[i].t=micros();return;}
}
static void dv_done(uint8_t d,bool gone) {for(unsigned i=0;i<8;i++) if(dv[i].d==d) {if(gone)dv[i].d=0;else dv[i].mounted=true;}}
void tuh_enum_descriptor_device_cb(uint8_t daddr,const tusb_desc_device_t *d) {
 tuh_bus_info_t bi={0};tuh_bus_info_get(daddr,&bi);dv_seen(daddr);
 /* The DWC2 driver starts split transactions for a low/full-speed device exactly when the root port runs at high speed. */
 bool split=hcd_port_speed_get(0)==TUSB_SPEED_HIGH&&bi.speed!=TUSB_SPEED_HIGH;
 char l[USBLOG_LINE];
 usblog_fmt_device(l,daddr,bi.hub_addr,bi.hub_port,speed_name(bi.speed),split,d->idVendor,d->idProduct,d->bDeviceClass,d->bDeviceSubClass,d->bDeviceProtocol,d->bMaxPacketSize0);
 lg(l);
}
/* TinyUSB calls this before SET_CONFIGURATION and before any class opens endpoints.
   Reject the entire configuration if any HID IN endpoint exceeds its receive buffer,
   including interfaces our keyboard mount callback would otherwise skip. */
bool tuh_enum_descriptor_configuration_cb(uint8_t daddr,uint8_t cfg_index,const tusb_desc_configuration_t *config) {
 (void)cfg_index;
 const uint8_t *p=(const uint8_t *)config;
 uint16_t total=(uint16_t)(p[2]|((uint16_t)p[3]<<8));
 if(total<sizeof(*config)||total>CFG_TUH_ENUMERATION_BUFSIZE) {char l[USBLOG_LINE];usblog_fmt_note(l,daddr,0xFF,"CONFIG REJECTED: bad total length");usblog_add(l);return false;}
 bool hid=false;uint8_t itf=0;
 for(uint16_t off=0;off<total;) {
  if(total-off<2||p[off]<2||p[off]>total-off) {char l[USBLOG_LINE];usblog_fmt_note(l,daddr,0xFF,"CONFIG REJECTED: bad descriptor length");usblog_add(l);return false;}
  uint8_t len=p[off],type=p[off+1];
  if(type==TUSB_DESC_INTERFACE) {
   if(len<sizeof(tusb_desc_interface_t)) return false;
   hid=p[off+5]==TUSB_CLASS_HID;
   {char l[USBLOG_LINE];itf=p[off+2];usblog_fmt_interface(l,daddr,itf,p[off+5],p[off+6],p[off+7],p[off+4]);usblog_add(l);}
  } else if(type==TUSB_DESC_ENDPOINT&&hid) {
   if(len<sizeof(tusb_desc_endpoint_t)) return false;
   uint16_t size=(uint16_t)(p[off+4]|((uint16_t)p[off+5]<<8))&0x7ffu;
   {char l[USBLOG_LINE];usblog_fmt_endpoint(l,daddr,itf,p[off+2],p[off+3],size,p[off+6]);usblog_add(l);}
   if((p[off+2]&0x80u)&&size>CFG_TUH_HID_EPIN_BUFSIZE) {
    log_line("USB host: HID IN packet exceeds receive buffer");
    {char l[USBLOG_LINE];usblog_fmt_note(l,daddr,itf,"CONFIG REJECTED: HID IN packet bigger than the 64 byte buffer");usblog_add(l);}
    return false; /* one log per rejected configuration, no mount/re-arm loop */
   }
  }
  off+=len;
 }
 return true;
}
void usb_irq(void) {
 uint32_t begin=micros();
 if(initialized&&!usb_wait_failed()) tuh_int_handler(0,true);
 usbhost_stats_irq((uint32_t)(micros()-begin));
 if(usb_wait_failed()) hcd_int_disable(0);
}
void tuh_mount_cb(uint8_t daddr) {
 uint16_t vid=0,pid=0;tuh_vid_pid_get(daddr,&vid,&pid);usbkbd_attached(vid,pid);
 char l[USBLOG_LINE];usblog_fmt_note(l,daddr,0xFF,"mounted (configuration set, every driver done)");lg(l);dv_done(daddr,false);
}
void tuh_umount_cb(uint8_t daddr) {
 usbkbd_device_gone(daddr);usbkbd_detached();
 char l[USBLOG_LINE];usblog_fmt_note(l,daddr,0xFF,"removed");lg(l);dv_done(daddr,true);
}
/* R4: arm the interrupt endpoint, retrying a refused submit a few times; if it stays refused the
   keyboard is removed (keys released, status truthful) rather than left ready with no poll. */
static void arm(uint8_t daddr,uint8_t idx) {
 for(unsigned n=0;n<3;n++) if(tuh_hid_receive_report(daddr,idx)) return;
 usbkbd_poll_failed(daddr,idx);
}
void tuh_hid_interface_skipped_cb(uint8_t daddr,uint8_t itf,bool capacity) {
 char line[USBLOG_LINE];usblog_fmt_note(line,daddr,itf,capacity?"HID slots full":"HID skipped: not a boot keyboard");lg(line);
}
void tuh_hid_mount_cb(uint8_t daddr,uint8_t idx,const uint8_t *desc,uint16_t len) {
 (void)desc;(void)len;
 /* TinyUSB's default protocol is boot, set before this callback for boot-subclass interfaces. */
 {tuh_itf_info_t info;char l[USBLOG_LINE];
  if(tuh_hid_itf_get_info(daddr,idx,&info)) {
   char t[60]="hid0 = interface 0, boot protocol 0, mode ";
   t[3]=(char)('0'+idx%10);t[17]=(char)('0'+info.desc.bInterfaceNumber%10);t[34]=(char)('0'+tuh_hid_interface_protocol(daddr,idx)%10);
   const char *m=tuh_hid_get_protocol(daddr,idx)==HID_PROTOCOL_BOOT?"boot":"report";unsigned n=42,k=0;while(m[k])t[n++]=m[k++];t[n]=0;
   usblog_fmt_note(l,daddr,0xFF,t);lg(l);}}
 bool boot_kbd=tuh_hid_interface_protocol(daddr,idx)==HID_ITF_PROTOCOL_KEYBOARD&&tuh_hid_get_protocol(daddr,idx)==HID_PROTOCOL_BOOT;
 if(usbkbd_hid_mounted(daddr,idx,boot_kbd)) arm(daddr,idx);
}
void tuh_hid_report_failed_cb(uint8_t daddr,uint8_t idx) {usbkbd_poll_failed(daddr,idx);}
void tuh_hid_umount_cb(uint8_t daddr,uint8_t idx) {usbkbd_hid_unmounted(daddr,idx);}
void tuh_hid_report_received_cb(uint8_t daddr,uint8_t idx,const uint8_t *report,uint16_t len) {
 if(usbkbd_report(daddr,idx,report,len)) arm(daddr,idx);
}
bool usb_mounted(void) {return false;}
bool usb_id_grounded(void) {return true;}
void usb_set_boot_note(const char *t) {(void)t;}
/* "KEYBOARD MODE", then the number of boot keyboards found once any is ready: 16 display columns. */
const char *usb_state(void) {
 static char t[20];if(!initialized) return init_fail;
 unsigned n=usbkbd_count();const char *s="KEYBOARD MODE";unsigned i=0;while(*s)t[i++]=*s++;
 if(n){t[i++]=' ';t[i++]=(char)('0'+(n>9?9:n));}
 t[i]=0;return t;
}
void usb_init(void) {
 uint32_t p[2]={3,3};
 if(!property(0x28001,p,2)) {init_fail="USB POWER";error("USB POWER");log_line(property_fail);return;}
 usbkbd_init();
 initialized=true;
 if(!tusb_init()) {usb_wait_report();hcd_int_disable(0);initialized=false;init_fail="TUSB HOST INIT";error("USB HOST FAILED");return;}
 usbkbd_host_up();usblog_add("host up: DWC2 host, TinyUSB 0.21, hubs and splits as configured");
}
void usb_reply_bytes(const uint8_t *p,uint32_t n) {(void)p;(void)n;}
void usb_reply(const char *s) {(void)s;}
bool usb_reply_pending(void) {return false;}
void usb_log(const char *s) {(void)s;}
/* A device that was read but never mounted within 4 s: say so once (TinyUSB drops a failed enumeration silently). */
static void watchdog(void) {
 usb_wait_report();
 if(usb_wait_failed()) {
  hcd_int_disable(0);initialized=false;init_fail="DWC2 TIMEOUT";
  for(uint8_t d=1;d<=CFG_TUH_DEVICE_MAX+CFG_TUH_HUB;d++) usbkbd_device_gone(d);
  return;
 }
 for(unsigned i=0;i<8;i++) if(dv[i].d&&!dv[i].mounted&&!dv[i].warned&&expired(dv[i].t,4000000)) {
  dv[i].warned=true;lg("USB stall absorbed: enumeration pending; kernel loop continues");char l[USBLOG_LINE];usblog_fmt_note(l,dv[i].d,0xFF,"NOT MOUNTED after 4 s: enumeration incomplete (see CTRL/retry lines)");lg(l);
 }
}
void tuh_control_retry_cb(uint8_t d,const tusb_control_request_t *r,xfer_result_t result,unsigned retry,unsigned delay_ms) {
 char l[USBLOG_LINE];
 const char *outcome=delay_ms?"failed":result==XFER_RESULT_SUCCESS?"recovered":result==XFER_RESULT_STALLED?"stalled":result==XFER_RESULT_ABORTED?"cancelled":"exhausted";
 usblog_fmt_control(l,d,r->bmRequestType,r->bRequest,r->wValue,r->wIndex,r->wLength,retry,delay_ms,outcome);lg(l);
}
void tuh_enumeration_result_cb(uint8_t d,uint8_t hub,uint8_t port,bool success) {
 (void)hub;(void)port;
 char l[USBLOG_LINE];
 usblog_fmt_note(l,d,0xFF,success?"enumeration complete":"NOT MOUNTED: enumeration failed (see CTRL/retry lines)");lg(l);
 if(success)dv_done(d,false);
}
void tuh_hub_retry_cb(uint8_t hub,uint8_t port,unsigned retry,const char *outcome) {
 char l[USBLOG_LINE];usblog_fmt_port_retry(l,hub,port,retry,outcome);lg(l);
}
void tuh_hub_poll_retry_cb(uint8_t hub,unsigned retry,unsigned delay_ms,const char *outcome) {
 char line[USBLOG_LINE];usblog_fmt_poll_retry(line,hub,retry,delay_ms,outcome);lg(line);
}
void usb_task(void) {
 if(!initialized) return;
 uint64_t begin=clock_us();
 /* Never wait for events; the host-only event budget also bounds a busy bus. */
 tuh_task_ext(0,false);
 watchdog();
 uint64_t end=clock_us();
 os_usb_work(begin,end);
 usbhost_stats_pass((uint32_t)(end-begin));
 usbhost_stats_task();
 /* Once per boot: retain evidence if a controller/callback still takes too long. */
 static bool warned;
 if(end-begin>=2000&&!warned) {
  warned=true;lg("USB stall absorbed: slow host pass excluded from OS heartbeat");
 }
}
/* The counters of attach/remove events, for the closing line of USB.TXT. */
void usb_host_events(uint32_t *attach,uint32_t *remove) {*attach=ev_attach;*remove=ev_remove;}
void service_io(void) {usb_task();protocol_task();}
uint32_t tusb_time_millis_api(void) {
 /* Keep TinyUSB deadlines monotonic across the 32-bit microsecond wrap. */
 static uint32_t last,ms,remainder;uint32_t now=micros(),delta=now-last;last=now;
 ms+=delta/1000;remainder+=delta%1000;ms+=remainder/1000;remainder%=1000;return ms;
}
bool usb_healthy(void) {return initialized&&!usb_wait_failed();}
