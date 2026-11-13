/* SPDX-License-Identifier: MIT; real host/HID/hub/glue, deterministic fake HCD. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbh.c"
#include "hub.c"
#include "usbkbd.h"
#include "input.h"
#include "usblog.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t raw;
static unsigned control_requests,mounted,status_lines,maximum;
static bool complete_reports=true,refuse;
static struct {unsigned opens,transfers;uint32_t last,min_gap;uint8_t *buffer;uint16_t length;} ep[6][256];
static unsigned reads_now,reads_micros;
uint32_t usb_wait_now(void) {reads_now++;return raw++;}
uint32_t micros(void) {reads_micros++;return raw++;} /* a timer read costs 1 us here, about what the Pi pays */
static uint32_t busy_us,passes_run;
bool usb_wait_failed(void) {return false;}
uint32_t tusb_time_millis_api(void) {return raw/1000;}
void tusb_time_delay_ms_api(uint32_t ms) {raw+=ms*1000;}
uint64_t clock_us(void) {static uint32_t last;static uint64_t time;time+=(uint32_t)(raw-last);last=raw;return time;}
void log_line(const char *s) {if(!strncmp(s,"USB win_ms=",11))status_lines++;}
void power_input(uint64_t time) {(void)time;}
void hcd_int_handler(uint8_t p,bool isr) {(void)p;(void)isr;}
void hcd_int_enable(uint8_t p) {(void)p;}
void hcd_int_disable(uint8_t p) {(void)p;}
bool hcd_init(uint8_t p,const tusb_rhport_init_t *i) {(void)p;(void)i;return true;}
void hcd_port_reset(uint8_t p) {(void)p;}
void hcd_port_reset_end(uint8_t p) {(void)p;}
bool hcd_port_connect_status(uint8_t p) {(void)p;return true;}
tusb_speed_t hcd_port_speed_get(uint8_t p) {(void)p;return TUSB_SPEED_HIGH;}
void hcd_device_close(uint8_t p,uint8_t d) {(void)p;(void)d;}
bool hcd_edpt_open(uint8_t p,uint8_t d,const tusb_desc_endpoint_t *e) {(void)p;ep[d][e->bEndpointAddress].opens++;return true;}
bool hcd_edpt_close(uint8_t p,uint8_t d,uint8_t e) {(void)p;(void)d;(void)e;return true;}
bool hcd_edpt_abort_xfer(uint8_t p,uint8_t d,uint8_t e) {(void)p;(void)d;(void)e;return true;}
bool hcd_edpt_clear_stall(uint8_t p,uint8_t d,uint8_t e) {(void)p;(void)d;(void)e;return true;}
bool hcd_setup_send(uint8_t p,uint8_t d,const uint8_t s[8]) {(void)p;(void)d;(void)s;control_requests++;return true;}
bool hcd_edpt_xfer(uint8_t p,uint8_t d,uint8_t e,uint8_t *b,uint16_t n) {
 (void)p;if(!e||e==0x80)return true;
 CHECK(e==0x81); /* Any skipped endpoint transfer fails the test immediately. */
 uint32_t gap=raw-ep[d][e].last;
 if(ep[d][e].transfers) {
  CHECK(gap>=(d==2?10000u:8000u));
  if(gap<ep[d][e].min_gap)ep[d][e].min_gap=gap;
 }
 ep[d][e].last=raw;ep[d][e].transfers++;ep[d][e].buffer=b;ep[d][e].length=n;
 raw+=120; /* Deliberately expensive fake HCD submit/split pair. */
 if(refuse)return false;
 usbhost_stats_token(d,e,true,d!=5,false);
 if(d!=5)usbhost_stats_token(d,e,true,true,true);
 if(complete_reports) {
  memset(b,0,n);if(d!=5&&n>=8)b[2]=ep[d][e].transfers&1?0x29:0; /* Escape down/up. */
  hcd_event_xfer_complete(d,e,n,XFER_RESULT_SUCCESS,false);
 }
 return true;
}
void tuh_control_retry_cb(uint8_t d,const tusb_control_request_t *r,xfer_result_t result,unsigned retry,unsigned delay) {
 (void)d;(void)r;(void)result;(void)retry;(void)delay;
}
void tuh_enumeration_result_cb(uint8_t d,uint8_t h,uint8_t p,bool success) {(void)d;(void)h;(void)p;CHECK(success);}
void tuh_hub_retry_cb(uint8_t h,uint8_t p,unsigned retry,const char *s) {(void)h;(void)p;(void)retry;(void)s;}
void tuh_hub_poll_retry_cb(uint8_t h,unsigned retry,unsigned delay,const char *s) {(void)h;(void)retry;(void)delay;(void)s;}
/* These callbacks are copied verbatim from the runtime driver by the runner. */
static void lg(const char *line) {usblog_add(line);}

static bool initialized=true;
static void watchdog(void) {}
static void os_usb_work(uint64_t begin,uint64_t end) {(void)begin;(void)end;}
#include "host-task.h"
static const uint8_t keyboard1[]={
 9,2,59,0,2,1,0,0x80,50,
 9,4,0,0,1,3,1,1,0, 9,0x21,0x11,1,0,1,0x22,0,0, 7,5,0x81,3,8,0,8,
 9,4,1,0,1,3,1,0,0, 9,0x21,0x11,1,0,1,0x22,0,0, 7,5,0x82,3,8,0,1
};
static const uint8_t keyboard2[]={
 9,2,59,0,2,1,0,0x80,50,
 9,4,0,0,1,3,1,1,0, 9,0x21,0x11,1,0,1,0x22,0,0, 7,5,0x81,3,8,0,10,
 9,4,1,0,1,3,0,0,0, 9,0x21,0x11,1,0,1,0x22,0,0, 7,5,0x82,3,8,0,1
};
static void enumerate(uint8_t d,const uint8_t *config,uint16_t vid,uint16_t pid) {
 usbh_device_t *dev=get_device(d);
 dev->connected=dev->addressed=1;dev->bus_info=(tuh_bus_info_t){.rhport=0,.hub_addr=5,.hub_port=d,.speed=TUSB_SPEED_FULL};
 dev->desc_device.idVendor=vid;dev->desc_device.idProduct=pid;
 memset(dev->itf2drv,0xff,sizeof(dev->itf2drv));memset(dev->ep2drv,0xff,sizeof(dev->ep2drv));
 CHECK(tuh_enum_descriptor_configuration_cb(d,0,(const void *)config));
 CHECK(enum_parse_configuration_desc(d,(const void *)config));
 CHECK(ep[d][0x81].opens==1&&ep[d][0x82].opens==0);
 CHECK(dev->itf2drv[1]==0xff); /* skipped interface gets no class setup, either */
 _usbh_data.enumerating_daddr=d;_usbh_data.dev0_bus=dev->bus_info;
 CHECK(hidh_set_config(d,0));
 for(unsigned i=0;i<12&&_usbh_data.ctrl_xfer_info.stage!=CONTROL_STAGE_IDLE;i++) {
  const tusb_control_request_t req=_usbh_epbuf.request;
  CHECK(req.wIndex==0); /* Never SET_IDLE, SET_PROTOCOL or read reports for interface 1. */
  usbh_control_xfer_cb(d,0,XFER_RESULT_SUCCESS,0);
  if(req.wLength)usbh_control_xfer_cb(d,req.bmRequestType&0x80,XFER_RESULT_SUCCESS,req.wLength);
  usbh_control_xfer_cb(d,req.bmRequestType&0x80?0:0x80,XFER_RESULT_SUCCESS,0);
 }
 CHECK(_usbh_data.enumerating_daddr==TUSB_INDEX_INVALID_8);
 mounted++;
}
static void pass(void) {
 uint32_t begin=raw;usb_task();uint32_t elapsed=raw-begin;busy_us+=elapsed;passes_run++;
 CHECK(elapsed<=USBHOST_TASK_BUDGET_US);
 if(elapsed>maximum)maximum=elapsed;
}
static void busy(void *unused) {(void)unused;raw+=200;}
int main(void) {
 input_init();usbkbd_init();
 CHECK(tuh_rhport_init(0,&(tusb_rhport_init_t){.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH}));
 _usbh_data.enumerating_daddr=TUSB_INDEX_INVALID_8;
 for(unsigned d=0;d<6;d++)for(unsigned e=0;e<256;e++)ep[d][e].min_gap=UINT32_MAX;
 // Real 1A40:0101 hub interface, configured fixture (port control is covered by retry tests).
 usbh_device_t *hub=get_device(5);hub->connected=hub->addressed=hub->configured=1;
 hub->bus_info=(tuh_bus_info_t){.rhport=0,.speed=TUSB_SPEED_HIGH};
 hub->desc_device.idVendor=0x1a40;hub->desc_device.idProduct=0x0101;
 const uint8_t hub_desc[]={9,4,0,0,1,9,0,1,0,7,5,0x81,3,1,0,1};
 CHECK(hub_open(0,5,(const void *)hub_desc,sizeof hub_desc)==sizeof hub_desc);
 hub->ep2drv[1][1]=BUILTIN_DRIVER_COUNT-1;
 get_hub_itf(5)->bNbrPorts=4;
 CHECK(hub_edpt_status_xfer(5));
 enumerate(1,keyboard1,0x1997,0x2433);enumerate(2,keyboard2,0x1a2c,0x2124);
 CHECK(mounted==2&&usbkbd_count()==2&&tuh_hid_itf_get_index(1,1)==TUSB_INDEX_INVALID_8);
 uint32_t instance=input_instance();CHECK(input_open(instance)==0);pass();
 struct input_event_rec events[64];bool escape=false;int count=input_read(instance,events,64);
 for(int i=0;i<count;i++)if(events[i].type==EV_KEY&&events[i].code==1)escape=true;
 CHECK(escape);input_close(instance);
 uint32_t begin=raw,stop=begin+1000000;
 busy_us=passes_run=reads_now=reads_micros=0;
 while((int32_t)(raw-stop)<0) {pass();raw+=50;}
 /* Benchmark: the host task over one second of a hub and two idle keyboards (the loop itself costs 50 us a spin here). */
 printf("host task over 1 s with a hub and two idle keyboards: %u passes, task_us=%u (%u%% of the second), %u stack timer reads, %u pass-gate reads\n",passes_run,busy_us,busy_us/10000,reads_now,reads_micros);
 CHECK(busy_us<100000); /* the same loop with no idle throttle and the old endpoint scan took 38 percent (383243 us); the rest here is the fake HCD paying 120 us a submit */
 unsigned first=ep[1][0x81].transfers,second=ep[2][0x81].transfers,hubs=ep[5][0x81].transfers;
 CHECK(first>=110&&first<=126&&second>=90&&second<=101&&hubs>=110&&hubs<=126);
 CHECK(!ep[1][0x82].transfers&&!ep[2][0x82].transfers);
 printf("fake HCD 1s: keyboard8=%u keyboard10=%u hub=%u skipped=0; min gaps=%u/%u us; max pass=%u/%u us\n",
        first,second,hubs,ep[1][0x81].min_gap,ep[2][0x81].min_gap,maximum,USBHOST_TASK_BUDGET_US);
 // Deferred event pressure must yield with work still queued under the time cap.
 for(unsigned i=0;i<16;i++)usbh_defer_func(busy,NULL,false);
 pass();CHECK(!osal_queue_empty(_usbh_q));while(!osal_queue_empty(_usbh_q))pass();
 // Late passes schedule from actual issue time, without a catch-up burst.
 raw+=1000000;first=ep[1][0x81].transfers;pass();CHECK(ep[1][0x81].transfers<=first+1);
 // Deadline comparisons survive the microsecond wrap.
 complete_reports=false;
 while(!osal_queue_empty(_usbh_q))pass();
 for(unsigned d=1;d<=2;d++) {
  get_device(d)->ep_status[1][1]=0;
  get_device(d)->poll[1][1].pending=false;
  get_device(d)->poll[1][1].next_us=UINT32_MAX-1000;
  ep[d][0x81].last=UINT32_MAX-1000-(d==2?10000:8000);
 }
 raw=UINT32_MAX-1001;
 uint8_t buf[8];CHECK(usbh_edpt_claim(1,0x81));CHECK(usbh_edpt_xfer(1,0x81,buf,8));
 raw=UINT32_MAX-999;pass();CHECK(ep[1][0x81].last>=UINT32_MAX-999);
 raw=2000;first=ep[1][0x81].transfers;pass();CHECK(ep[1][0x81].transfers==first);
 raw=8000;pass();
 // Aborting/unplugging a deferred transfer cancels it before stack buffer expiry.
 get_device(1)->ep_status[1][1]=0;
 CHECK(usbh_edpt_claim(1,0x81));CHECK(usbh_edpt_xfer(1,0x81,buf,8));
 CHECK(tuh_edpt_abort_xfer(1,0x81));first=ep[1][0x81].transfers;raw+=20000;pass();CHECK(ep[1][0x81].transfers==first);
 usbh_device_close(0,1);get_device(1)->connected=0;
 first=ep[1][0x81].transfers;raw+=20000;pass();CHECK(ep[1][0x81].transfers==first);
 // Latest counters survive a full descriptor log and include five-second status output.
 raw+=5000000;usbhost_stats_task();CHECK(status_lines>0);
 for(unsigned i=0;i<200;i++)usblog_add("descriptor filler ..............................................................");
 uint32_t bytes_log;const char *text=usblog_text(&bytes_log);
 CHECK(bytes_log<=USBLOG_MAX&&strstr(text,"USB win_ms=")&&strstr(text,"submit=")&&strstr(text,"max_pass="));
 CHECK(control_requests==6); /* three class setup requests per accepted keyboard only */
 // Refused deferred hub submissions retain the existing bounded recovery.
 get_device(2)->connected=0;
 hub->ep_status[1][1]=0;hub->poll[1][1].issued=true;hub->poll[1][1].next_us=raw+8000;
 get_hub_itf(5)->poll_active=false;get_hub_itf(5)->want_poll=true;
 ep[5][0x81].last=raw;refuse=true;unsigned before_hub=ep[5][0x81].transfers;
 pass();CHECK(hub->poll[1][1].pending&&ep[5][0x81].transfers==before_hub);
 raw+=8000;pass();CHECK(get_hub_itf(5)->poll_retries==1);
 for(unsigned retry=1;retry<=USBHOST_POLL_RETRIES;retry++) {
  raw=get_hub_itf(5)->poll_retry_at*1000;pass();
 }
 CHECK(ep[5][0x81].transfers==before_hub+4&&!hub->connected&&root_retry.retry_port);
 printf("all fake-HCD passes including event pressure: max=%u/%u us\n",maximum,USBHOST_TASK_BUDGET_US);
 puts("skip-before-open, successful-report spacing, Escape, hub floor, event budget, late pass, wrap, abort, unplug and stats OK");
 return 0;
}
