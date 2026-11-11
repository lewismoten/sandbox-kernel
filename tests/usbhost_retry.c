/* SPDX-License-Identifier: MIT; real patched stack, fake controller, no MMIO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef assert
#define assert(x) do {if (!(x)) {fprintf(stderr,"check failed at line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
#include "usbh.c"
#include "hub.c"
#ifdef USBHOST_CLOCK_TEST
#include "host-clock.h"
#endif
static uint32_t now;
static unsigned setups, callbacks, retry_logs, enum_success, port_logs, polls;
static bool submit_ok=true;
static bool poll_submit_ok=true;
static unsigned poll_logs, poll_exhausted, poll_recovered, root_resets, root_exhausted;
static xfer_result_t completed;
uint32_t tusb_time_millis_api(void) {return now;}
void tusb_time_delay_ms_api(uint32_t ms) {now+=ms;}
void hcd_int_handler(uint8_t p,bool isr) {(void)p;(void)isr;}
bool hcd_edpt_close(uint8_t p,uint8_t d,uint8_t e) {(void)p;(void)d;(void)e;return true;}
void hcd_int_enable(uint8_t p) {(void)p;}
void hcd_int_disable(uint8_t p) {(void)p;}
bool hcd_init(uint8_t p,const tusb_rhport_init_t *i) {(void)p;(void)i;return true;}
void hcd_port_reset(uint8_t p) {(void)p;root_resets++;}
void hcd_port_reset_end(uint8_t p) {(void)p;}
bool hcd_port_connect_status(uint8_t p) {(void)p;return true;}
tusb_speed_t hcd_port_speed_get(uint8_t p) {(void)p;return TUSB_SPEED_HIGH;}
void hcd_device_close(uint8_t p,uint8_t d) {(void)p;(void)d;}
bool hcd_edpt_open(uint8_t p,uint8_t d,const tusb_desc_endpoint_t *e) {(void)p;(void)d;(void)e;return true;}
bool hcd_edpt_abort_xfer(uint8_t p,uint8_t d,uint8_t e) {(void)p;(void)d;(void)e;return true;}
bool hcd_edpt_clear_stall(uint8_t p,uint8_t d,uint8_t e) {(void)p;(void)d;(void)e;return true;}
bool hcd_setup_send(uint8_t p,uint8_t d,const uint8_t s[8]) {(void)p;(void)d;(void)s;setups++;return submit_ok;}
bool hcd_edpt_xfer(uint8_t p,uint8_t d,uint8_t e,uint8_t *b,uint16_t n) {(void)p;(void)d;(void)b;(void)n;if(e==0x81){polls++;return poll_submit_ok;}return submit_ok;}
void tuh_control_retry_cb(uint8_t d,const tusb_control_request_t *r,xfer_result_t result,unsigned retry,unsigned delay) {
 (void)d;assert(r->bRequest==TUSB_REQ_GET_DESCRIPTOR||r->bRequest==HUB_REQUEST_GET_STATUS);
 assert(retry<=3);if(delay)assert(delay==usbhost_backoff(retry));(void)result;retry_logs++;
}
void tuh_enumeration_result_cb(uint8_t d,uint8_t h,uint8_t p,bool success) {(void)d;(void)h;(void)p;if(success)enum_success++;}
void tuh_hub_retry_cb(uint8_t h,uint8_t p,unsigned retry,const char *s) {assert(((h==5&&p==2)||(h==0&&p==0))&&retry<=2&&s);if(!h&&!strcmp(s,"exhausted"))root_exhausted++;port_logs++;}
void tuh_hub_poll_retry_cb(uint8_t hub,unsigned retry,unsigned delay,const char *outcome) {
 assert(hub==5&&retry>0&&retry<=USBHOST_POLL_RETRIES);
 if(delay) {assert(delay==usbhost_backoff(retry));assert(!strcmp(outcome,"refused"));}
 else if(!strcmp(outcome,"recovered")) poll_recovered++;
 else {assert(!strcmp(outcome,"exhausted: reset"));poll_exhausted++;}
 poll_logs++;
}
static void done(tuh_xfer_t *x) {callbacks++;completed=x->result;}
static void reset(void) {
 assert(tuh_rhport_init(0,&(tusb_rhport_init_t){.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH}));
 _usbh_data.enumerating_daddr=TUSB_INDEX_INVALID_8;
 for(unsigned i=0;i<TOTAL_DEVICES;i++) {_usbh_devices[i].connected=1;_usbh_devices[i].bus_info.rhport=0;}
}
static void start(void) {
 tusb_control_request_t r={.bmRequestType=0x80,.bRequest=TUSB_REQ_GET_DESCRIPTOR,.wValue=0x100,.wLength=0};
 tuh_xfer_t x={.daddr=1,.setup=&r,.complete_cb=done};assert(tuh_control_xfer(&x));
}
static void finish(void) {
 assert(usbh_control_xfer_cb(1,0,XFER_RESULT_SUCCESS,0));
 assert(usbh_control_xfer_cb(1,0x80,XFER_RESULT_SUCCESS,0));
}
static hub_interface_t *poll_fixture(void) {
 hub_init();tu_memclr(_usbh_devices,sizeof(_usbh_devices));
 _usbh_data.enumerating_daddr=TUSB_INDEX_INVALID_8;
 _usbh_data.ctrl_xfer_info.stage=CONTROL_STAGE_IDLE;
 _usbh_data.call_after.func=NULL;
 _usbh_devices[4].connected=1;
 _usbh_devices[4].bus_info=(tuh_bus_info_t){.rhport=0,.hub_addr=0,.hub_port=0};
 for(unsigned driver=0;driver<TOTAL_DRIVER_COUNT;driver++) {
  if(get_driver(driver)->xfer_cb==hub_xfer_cb) _usbh_devices[4].ep2drv[1][1]=driver;
 }
 hub_interface_t *hub=get_hub_itf(5);hub->ep_in=0x81;hub->bNbrPorts=4;
 get_hub_epbuf(5)->status_change[0]=0;
 hub_edpt_status_xfer(5);poll_submit_ok=false;return hub;
}
static void poll_retry_tests(void) {
 hub_interface_t *hub=poll_fixture();
 _usbh_devices[0].connected=1;
 _usbh_devices[0].bus_info=(tuh_bus_info_t){.rhport=0,.hub_addr=5,.hub_port=2};
 unsigned before=polls, logs=poll_logs;
 for(unsigned call=0;call<10000;call++) tuh_task_ext(0,false);
 assert(polls==before+1&&poll_logs==logs+1&&hub->poll_retries==1);
 for(unsigned retry=1;retry<=USBHOST_POLL_RETRIES;retry++) {
  now+=usbhost_backoff(retry)-1;
  for(unsigned call=0;call<10000;call++) tuh_task_ext(0,false);
  assert(polls==before+retry&&poll_logs==logs+retry);
  now++;tuh_task_ext(0,false);assert(polls==before+retry+1);
 }
 assert(poll_exhausted==1&&!hub->ep_in&&!_usbh_devices[4].connected);
 assert(!_usbh_devices[0].connected&&!hub->poll_retries&&!hub->poll_retry_at);
 assert(root_retry.retry_port&&root_retry.retries[0]==1);
 unsigned resets=root_resets;
 now+=199;tuh_task_ext(0,false);assert(root_resets==resets);
 now++;tuh_task_ext(0,false);assert(_usbh_data.enumerating_daddr==0&&!root_retry.retry_port);
 now+=ENUM_DEBOUNCING_DELAY_MS+1;tuh_task_ext(0,false);assert(root_resets==resets+1);
 enum_full_complete(false);assert(root_retry.retry_port&&root_retry.retries[0]==2);
 now+=399;tuh_task_ext(0,false);assert(_usbh_data.enumerating_daddr==TUSB_INDEX_INVALID_8);
 now++;tuh_task_ext(0,false);assert(_usbh_data.enumerating_daddr==0);
 enum_full_complete(false);assert(!root_retry.retry_port&&root_retry.retries[0]==2&&root_exhausted==1);
 before=polls;resets=root_resets;logs=poll_logs;
 unsigned recovery_logs=port_logs;
 for(unsigned call=0;call<10000;call++){now++;tuh_task_ext(0,false);}
 assert(polls==before&&root_resets==resets&&poll_logs==logs&&port_logs==recovery_logs);
 enum_full_complete(true);assert(root_retry.retries[0]==0);
 hub_enum_result(0,0,false);assert(root_retry.retry_port);
 enum_full_complete(true);assert(!root_retry.retries[0]&&!root_retry.retry_port&&!root_retry.retry_at);
 hub_enum_result(0,0,false);assert(root_retry.retry_port);
 _usbh_data.attach_debouncing_bm=0;
 hcd_event_t removed={.rhport=0,.event_id=HCD_EVENT_DEVICE_REMOVE,
                      .connection={.hub_addr=0,.hub_port=0}};
 hcd_event_handler(&removed,false);tuh_task_ext(0,false);
 assert(!root_retry.retries[0]&&!root_retry.retry_port&&!root_retry.retry_at);
 resets=root_resets;logs=port_logs;now+=1000;
 for(unsigned call=0;call<10000;call++) tuh_task_ext(0,false);
 assert(root_resets==resets&&port_logs==logs);
 hub_enum_result(0,0,false);assert(root_retry.retries[0]==1);
 hub=poll_fixture();now=UINT32_MAX-10;before=polls;logs=poll_logs;
 tuh_task_ext(0,false);assert(polls==before+1);
 now=8;tuh_task_ext(0,false);assert(polls==before+1);
 poll_submit_ok=true;now=9;tuh_task_ext(0,false);
 assert(polls==before+2&&hub->poll_active&&!hub->want_poll);
 assert(!hub->poll_retries&&!hub->poll_retry_at&&poll_recovered==1&&poll_logs==logs+2);
 hcd_event_xfer_complete(5,0x81,1,XFER_RESULT_SUCCESS,false);
 poll_submit_ok=false;tuh_task_ext(0,false);
 assert(hub->poll_retries==1&&hub->poll_retry_at==now+20);
 hub_close(5);logs=poll_logs;before=polls;now+=1000;tuh_task_ext(0,false);
 assert(!hub->poll_retries&&!hub->poll_retry_at&&poll_logs==logs&&polls==before);
 hub=poll_fixture();before=polls;logs=poll_logs;tuh_task_ext(0,false);
 now+=100000;
 for(unsigned call=0;call<10000;call++) tuh_task_ext(0,false);
 assert(polls==before+2&&poll_logs==logs+2&&hub->poll_retries==2&&hub->poll_retry_at==now+40);
 poll_submit_ok=true;now+=40;tuh_task_ext(0,false);
 assert(hub->poll_active&&!hub->poll_retries&&!hub->poll_retry_at&&poll_recovered==2);
 hub=poll_fixture();hub->want_poll=false;
 _usbh_devices[0].connected=1;
 _usbh_devices[0].bus_info=(tuh_bus_info_t){.rhport=0,.hub_addr=5,.hub_port=2};
 usbh_hub_poll_failed(1);
 assert(!_usbh_devices[0].connected&&hub->ep_in&&hub->retry_port==2&&hub->retries[2]==1);
 unsigned setups_before=setups;
 now+=199;tuh_task_ext(0,false);assert(setups==setups_before&&hub->retry_port==2);
 now++;tuh_task_ext(0,false);assert(_usbh_data.enumerating_daddr==0&&!hub->retry_port);
 now+=ENUM_DEBOUNCING_DELAY_MS+1;tuh_task_ext(0,false);
 assert(_usbh_epbuf.request.bRequest==HUB_REQUEST_GET_STATUS&&_usbh_epbuf.request.wIndex==2);
}
int main(void) {
#ifdef USBHOST_CLOCK_TEST
 raw_us=UINT32_MAX-499;uint32_t before_wrap=host_clock_from_micros();
 raw_us=500;assert(host_clock_from_micros()==before_wrap+1);
 raw_us=1500;assert(host_clock_from_micros()==before_wrap+2);
#endif
 reset();
 // A controller can accept SETUP and never complete it for four seconds.
 // Every main-loop pass still returns so OS ticks/health calls can run.
 start();unsigned os_ticks=0, stalled_at=now;
 for(unsigned ms=0;ms<4000;ms++) {
  now=stalled_at+ms;
  tuh_task_ext(0,false);
  assert(now==stalled_at+ms&&callbacks==0&&setups==1);
  os_ticks++;
 }
 assert(os_ticks==4000);
 now=stalled_at+4000;finish();assert(callbacks==1);
 callbacks=0;setups=0;
 // A burst must leave events for later passes, rather than draining forever.
 for(unsigned i=0;i<8;i++) {
  hcd_event_t event={.rhport=0,.event_id=HCD_EVENT_XFER_COMPLETE,.dev_addr=1,
                   .xfer_complete={.ep_addr=0,.result=XFER_RESULT_SUCCESS}};
  hcd_event_handler(&event,false);
 }
 tuh_task_ext(0,false);assert(!osal_queue_empty(_usbh_q));
 tuh_task_ext(0,false);assert(osal_queue_empty(_usbh_q));
 start();assert(setups==1);
 for(unsigned n=1;n<=3;n++) {
  assert(usbh_control_xfer_cb(1,0,XFER_RESULT_FAILED,0));
  unsigned before=setups;assert(callbacks==0);
  now+=usbhost_backoff(n)-1;control_retry_task();assert(setups==before);
  now++;control_retry_task();assert(setups==before+1);
 }
 finish();assert(callbacks==1&&completed==XFER_RESULT_SUCCESS&&retry_logs==4);
 // Refused setup submissions also follow backoff and eventually notify the caller.
 submit_ok=false;start();assert(callbacks==1);
 for(unsigned n=1;n<=3;n++){now+=usbhost_backoff(n);control_retry_task();}
 assert(callbacks==2&&completed==XFER_RESULT_FAILED);
 submit_ok=true;start();usbh_control_xfer_cb(1,0,XFER_RESULT_STALLED,0);
 assert(callbacks==3&&completed==XFER_RESULT_STALLED);
 // Optional string STALL must continue with configuration, including no-hub devices.
 _usbh_data.enumerating_daddr=1;
 tusb_control_request_t string_req={.bmRequestType=0x80,.bRequest=TUSB_REQ_GET_DESCRIPTOR,.wValue=0x300};
 tuh_xfer_t string_fail={.daddr=1,.setup=&string_req,.result=XFER_RESULT_STALLED};
 process_enumeration(&string_fail);
 assert(_usbh_epbuf.request.wValue==0x200&&_usbh_data.enumerating_daddr==1);
 _usbh_data.enumerating_daddr=TUSB_INDEX_INVALID_8;
 // Release this intentionally unfinished fixture without consuming descriptor data.
 _usbh_data.ctrl_xfer_info.complete_cb=done;control_xfer_complete(1,XFER_RESULT_ABORTED);
 callbacks--;
 // Cancellation while waiting must not send a late setup or invoke a callback twice.
 start();usbh_control_xfer_cb(1,0,XFER_RESULT_FAILED,0);usbh_device_close(0,1);
 unsigned before=setups;now+=100;control_retry_task();assert(setups==before&&callbacks==4);
 now=UINT32_MAX-10;start();usbh_control_xfer_cb(1,0,XFER_RESULT_FAILED,0);
 before=setups;now=8;control_retry_task();assert(setups==before);
 now=9;control_retry_task();assert(setups==before+1);finish();
 assert(usbhost_due(5,UINT32_MAX-3));assert(!usbhost_due(UINT32_MAX-3,5));
 // Hub bitmap arrives while another device owns shared enumeration state.
 hub_interface_t *h=get_hub_itf(5);h->ep_in=0x81;h->bNbrPorts=4;
 get_hub_epbuf(5)->status_change[0]=(1<<1)|(1<<3);
 _usbh_data.enumerating_daddr=1;
 hub_xfer_cb(5,0x81,XFER_RESULT_SUCCESS,1);
 before=setups;tuh_task_ext(0,false);assert(setups==before&&h->pending==10);
 _usbh_data.enumerating_daddr=TUSB_INDEX_INVALID_8;
 tuh_task_ext(0,false);assert(setups==before+1&&h->pending==8);
 assert(_usbh_epbuf.request.wIndex==1);
 // Completing a no-change port resumes the saved bitmap before another interrupt poll.
 get_hub_itf(5)->port_status.change.value=0;
 usbh_control_xfer_cb(5,0,XFER_RESULT_SUCCESS,0);
 usbh_control_xfer_cb(5,0x80,XFER_RESULT_SUCCESS,4);
 usbh_control_xfer_cb(5,0,XFER_RESULT_SUCCESS,0);
 tuh_task_ext(0,false);assert(_usbh_epbuf.request.wIndex==3&&h->pending==0);
 // Bounded port recovery and cancellation on hub unplug.
 hub_enum_result(5,2,false);assert(h->retry_port==2&&h->retries[2]==1);
 h->retry_port=0;hub_enum_result(5,2,false);assert(h->retries[2]==2);
 h->retry_port=0;hub_enum_result(5,2,false);assert(h->retry_port==0&&port_logs==3);
 hub_enum_result(5,2,true);assert(h->retries[2]==0&&port_logs==4);
 // Drive a real failed enumeration through cleanup, backoff, debounce and PORT_RESET.
 control_xfer_complete(5,XFER_RESULT_SUCCESS);
 _usbh_data.enumerating_daddr=1;
 _usbh_devices[0].connected=1;
 _usbh_devices[0].bus_info.hub_addr=5;_usbh_devices[0].bus_info.hub_port=2;
 _usbh_data.dev0_bus=(tuh_bus_info_t){.rhport=0,.hub_addr=5,.hub_port=2};
 start();usbh_control_xfer_cb(1,0,XFER_RESULT_STALLED,0);
 assert(!_usbh_devices[0].connected&&h->retry_port==2);
 before=setups;now+=199;tuh_task_ext(0,false);assert(setups==before&&_usbh_data.enumerating_daddr==TUSB_INDEX_INVALID_8);
 now++;tuh_task_ext(0,false);assert(_usbh_data.enumerating_daddr==0&&h->retry_port==0);
 now+=ENUM_DEBOUNCING_DELAY_MS+1;tuh_task_ext(0,false);
 assert(_usbh_epbuf.request.bRequest==HUB_REQUEST_GET_STATUS&&_usbh_epbuf.request.wIndex==2);
 ((hub_port_status_response_t *)get_hub_epbuf(5)->ctrl_buf)->status.connection=1;
 usbh_control_xfer_cb(5,0,XFER_RESULT_SUCCESS,0);
 usbh_control_xfer_cb(5,0x80,XFER_RESULT_SUCCESS,4);
 usbh_control_xfer_cb(5,0,XFER_RESULT_SUCCESS,0);
 assert(_usbh_epbuf.request.bRequest==HUB_REQUEST_SET_FEATURE&&_usbh_epbuf.request.wValue==HUB_FEATURE_PORT_RESET);
 hub_close(5);assert(h->retry_port==0&&h->pending==0);
 // Hubs get explicit completion even though upstream omits tuh_mount_cb for them.
 _usbh_data.enumerating_daddr=5;_usbh_data.dev0_bus=(tuh_bus_info_t){0};
 enum_full_complete(true);assert(enum_success==1);
 poll_retry_tests();
 puts("USB host: timed control/poll retries, fixed-time refusal bound, exhaustion/reset, recovery, cancellation and wrap OK");
 return 0;
}
