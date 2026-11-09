/* SPDX-License-Identifier: MIT; descriptors and original TinyUSB board glue. */
#include "kernel.h"
#include "tusb.h"
#include "device/dcd.h"
extern volatile bool usb_reset_seen;
static bool initialized,inside,suspended;
static char replies[512],logs[4096];
static unsigned rp,wp,lr,lw;
static uint32_t dropped;
static const tusb_desc_device_t device={
 .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x0200,
 .bDeviceClass=TUSB_CLASS_MISC,.bDeviceSubClass=MISC_SUBCLASS_COMMON,.bDeviceProtocol=MISC_PROTOCOL_IAD,
 .bMaxPacketSize0=64,.idVendor=0xcafe,.idProduct=0x4011,.bcdDevice=0x0001,
 .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1};
const uint8_t *tud_descriptor_device_cb(void) {return (const uint8_t *)&device;}
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN+TUD_CDC_DESC_LEN)
#define CONFIG(speed) TUD_CONFIG_DESCRIPTOR(1,2,0,CONFIG_LEN,0,100),TUD_CDC_DESCRIPTOR(0,4,0x81,8,0x02,0x82,speed)
static const uint8_t hs[]={CONFIG(512)},fs[]={CONFIG(64)};
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {(void)index;return tud_speed_get()==TUSB_SPEED_HIGH?hs:fs;}
static const tusb_desc_device_qualifier_t qualifier={.bLength=sizeof(tusb_desc_device_qualifier_t),.bDescriptorType=TUSB_DESC_DEVICE_QUALIFIER,.bcdUSB=0x0200,.bDeviceClass=TUSB_CLASS_MISC,.bDeviceSubClass=MISC_SUBCLASS_COMMON,.bDeviceProtocol=MISC_PROTOCOL_IAD,.bMaxPacketSize0=64,.bNumConfigurations=1};
const uint8_t *tud_descriptor_device_qualifier_cb(void) {return (const uint8_t *)&qualifier;}
const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t index) {static uint8_t b[CONFIG_LEN];(void)index;memcpy(b,tud_speed_get()==TUSB_SPEED_HIGH?fs:hs,CONFIG_LEN);b[1]=TUSB_DESC_OTHER_SPEED_CONFIG;return b;}
const uint16_t *tud_descriptor_string_cb(uint8_t index,uint16_t lang) {
 static uint16_t b[64];(void)lang;const char *s="";
 if(!index) {b[0]=0x0304;b[1]=0x0409;return b;}
 if(index==1)s="sandbox-kernel";else if(index==2)s="Zero W M0 recovery";else if(index==3)s="M0-development";else if(index==4)s="Recovery CDC";else return NULL;
 unsigned n=strlen(s);for(unsigned i=0;i<n;i++)b[1+i]=(uint8_t)s[i];b[0]=(uint16_t)(0x300|((n+1)*2));return b;
}
void usb_irq(void) {if(initialized)tud_int_handler(0);}
void tud_mount_cb(void) {suspended=false;log_line("USB enumerated (reader separate)");}
void tud_umount_cb(void) {protocol_reset();log_line("USB unmounted");}
void tud_suspend_cb(bool remote) {(void)remote;suspended=true;protocol_reset();}
void tud_resume_cb(void) {suspended=false;}
/* Boot notes go out 1 s after a reader opens: bytes sent right as the port
   opens can be lost on the host side. */
static bool summary_due; static uint32_t summary_at;
void tud_cdc_line_state_cb(uint8_t itf,bool dtr,bool rts) {(void)itf;(void)rts;if(!dtr)protocol_reset();log_line(dtr?"USB reader open":"USB reader closed");if(dtr){summary_at=micros();summary_due=true;}}
/* Called on every bus reset, before queued task events. No disk work in ISR. */
void tud_event_hook_cb(uint8_t rhport,uint32_t eventid,bool in_isr) {
 (void)rhport;(void)in_isr; if(eventid==DCD_EVENT_BUS_RESET) {extern volatile bool usb_reset_seen;usb_reset_seen=true;}
}
static const char *init_fail="unavailable";
static char fail_text[32];
const char *usb_state(void) {if(!initialized)return init_fail;if(suspended)return "suspended";if(!tud_mounted())return "disconnected";return tud_cdc_connected()?"reader":"enumerated";}
void usb_init(void) {
 uint32_t p[2]={3,3};if(!property(0x28001,p,2)) {{const char *a="USB POWER ",*b=property_fail;unsigned k=0;while(*a&&k<30)fail_text[k++]=*a++;while(*b&&k<30)fail_text[k++]=*b++;fail_text[k]=0;} init_fail=fail_text;error("USB POWER");log_line(property_fail);return;}
 /* Force device role; internal UTMI+ PHY selected by TinyUSB. */
 REG(0x2098000c)=(REG(0x2098000c)&~(1u<<29))|(1u<<30);
 initialized=true;if(!tusb_init()) {initialized=false;init_fail="TUSB INIT";error("TUSB INIT");}
}
void usb_reply(const char *s) {while(*s) {if(wp-rp>=sizeof(replies)) {error("Reply queue full");return;}replies[wp++%sizeof(replies)]=*s++;}}
bool usb_reply_pending(void) {return wp!=rp || (initialized&&tud_cdc_write_available()!=CFG_TUD_CDC_TX_BUFSIZE);}
void usb_log(const char *s) {
 size_t n=strlen(s);if(n+5>sizeof(logs)-(lw-lr)) {dropped++;return;}
 const char *prefix="LOG ";while(*prefix)logs[lw++%sizeof(logs)]=*prefix++;
 while(*s)logs[lw++%sizeof(logs)]=*s++;
 logs[lw++%sizeof(logs)]='\n';
}
extern bool protocol_busy(void);
/* OS serial output (docs/serial.md): whole "OS ..." lines join the same ring as
   LOG lines, so the two never mix inside a line. Lines that do not fit wait. */
static void os_lines(void) {
 char l[SERIAL_LINE_MAX];
 while(sizeof(logs)-(lw-lr)>SERIAL_LINE_MAX) {
  uint32_t n=serial_tx_line(l);if(!n)break;
  for(uint32_t i=0;i<n;i++)logs[lw++%sizeof(logs)]=l[i];
 }
}
void usb_task(void) {
 if(!initialized||inside)return;inside=true;if(usb_reset_seen) {rp=wp=0;}tud_task();
 serial_set_reader(tud_mounted()&&tud_cdc_connected());
 if(!tud_mounted()) {inside=false;return;}
 while(rp!=wp&&tud_cdc_write_available()) {tud_cdc_write_char(replies[rp++%sizeof(replies)]);}
 if(!protocol_busy()&&rp==wp&&tud_cdc_connected()) {
  if(summary_due&&expired(summary_at,1000000)) {summary_due=false;boot_summary();}
  os_lines();
  while(lr!=lw&&tud_cdc_write_available())tud_cdc_write_char(logs[lr++%sizeof(logs)]);
  if(dropped&&lr==lw&&tud_cdc_write_available()>=26) {tud_cdc_write_str("LOG dropped (slow reader)\n");dropped=0;}
 }
 tud_cdc_write_flush();inside=false;
}
void service_io(void) {usb_task();protocol_task();}
uint32_t tusb_time_millis_api(void) {return micros()/1000;}

bool usb_healthy(void) {return initialized;}
