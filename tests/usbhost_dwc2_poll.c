/* SPDX-License-Identifier: MIT; actual PIO split retry path over fake registers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/tusb_common.h"
#include "portable/synopsys/dwc2/dwc2_type.h"
static dwc2_regs_t regs;
#define DWC2_REG(_port) (&regs)
#undef dwc2_int_set
#define dwc2_int_set(_port,_role,_on) ((void)0)
#include "portable/synopsys/dwc2/dwc2_common.c"
#include "portable/synopsys/dwc2/hcd_dwc2.c"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t raw,polls,ss,cs;
uint32_t micros(void) {return raw++;}
void idle_wait(uint32_t us) {raw+=us;}
void log_line(const char *s) {(void)s;}
void hcd_event_handler(const hcd_event_t *e,bool isr) {(void)e;(void)isr;}
bool tuh_bus_info_get(uint8_t d,tuh_bus_info_t *b) {
 *b=(tuh_bus_info_t){.rhport=0,.hub_addr=5,.hub_port=d,.speed=d==3?TUSB_SPEED_HIGH:TUSB_SPEED_FULL};return true;
}
void usbhost_stats_token(uint8_t d,uint8_t e,bool interrupt,bool split,bool complete) {
 (void)d;(void)e;if(interrupt&&(!split||!complete))polls++;
 if(split){if(complete)cs++;else ss++;}
}
static void put(volatile const uint32_t *p,uint32_t v) {*(volatile uint32_t *)(uintptr_t)p=v;}
static void task(void) {uint32_t start=raw;usbhost_dwc2_task(start);CHECK(raw-start<=USBHOST_TASK_BUDGET_US);}
static void nak(uint8_t id) {
 dwc2_channel_t *ch=&regs.channel[id];
 ch->hcsplt&=~HCSPLT_COMPLSPLT;
 channel_xfer_in_retry(&regs,id,HCINT_NAK);
 CHECK(!(_hcd_data.xfer[id].allocated));CHECK(!(regs.gintmsk&GINTSTS_SOF));
}
int main(void) {
 dwc2_ghwcfg2_t hw={.num_host_ch=7};put(&regs.ghwcfg2,hw.value);
 dwc2_hptxsts_t queue={.req_queue_available=8};put(&regs.hptxsts,queue.value);put(&regs.hnptxsts,queue.value);
 tusb_desc_endpoint_t desc={.bLength=7,.bDescriptorType=TUSB_DESC_ENDPOINT,.bEndpointAddress=0x81,
                          .bmAttributes={.xfer=TUSB_XFER_INTERRUPT},.wMaxPacketSize=8,.bInterval=8};
 uint8_t buf[8];CHECK(hcd_edpt_open(0,1,&desc));CHECK(hcd_edpt_xfer(0,1,0x81,buf,sizeof buf));
 uint8_t ep_id=edpt_find_opened(1,1,1),ch_id=channel_find_enabled(&regs,1,1,1);
 CHECK(ch_id<8&&ss==1&&polls==1&&cs==0);
 uint32_t at=_hcd_data.edpt[ep_id].interrupt_at;
 CHECK(!handle_channel_in_slave(&regs,ch_id,HCINT_ACK));CHECK(cs==1&&ss==1);
 nak(ch_id);CHECK(_hcd_data.edpt[ep_id].interrupt_pending);
 // Fixed-time calls and real elapsed SOFs must not restart or enable SOF IRQ.
 for(unsigned i=0;i<100;i++){raw=at-1000;task();CHECK(polls==1);}
 raw=at-100;task();CHECK(polls==1);
 raw=at;task();CHECK(polls==2&&ss==2&&!(regs.gintmsk&GINTSTS_SOF));
 ch_id=channel_find_enabled(&regs,1,1,1);CHECK(ch_id<8);nak(ch_id);
 at=_hcd_data.edpt[ep_id].interrupt_at;
 raw=at+100000;task();CHECK(polls==3); /* Late service: one retry only. */
 ch_id=channel_find_enabled(&regs,1,1,1);nak(ch_id);
 CHECK(hcd_edpt_abort_xfer(0,1,0x81));raw+=100000;task();CHECK(polls==3);
 CHECK(hcd_edpt_xfer(0,1,0x81,buf,sizeof buf));ch_id=channel_find_enabled(&regs,1,1,1);nak(ch_id);
 CHECK(hcd_edpt_close(0,1,0x81));raw+=100000;task();CHECK(polls==4);
 // bInterval=1 is floored to 8 ms at FS and HS; 10 ms remains 10 ms.
 desc.bInterval=1;CHECK(hcd_edpt_open(0,2,&desc));
 ep_id=edpt_find_opened(2,1,1);CHECK(_hcd_data.edpt[ep_id].uframe_interval==64);
 CHECK(hcd_edpt_open(0,3,&desc));ep_id=edpt_find_opened(3,1,1);CHECK(_hcd_data.edpt[ep_id].uframe_interval==64);
 desc.bInterval=10;CHECK(hcd_edpt_open(0,4,&desc));ep_id=edpt_find_opened(4,1,1);CHECK(_hcd_data.edpt[ep_id].uframe_interval==80);
 // Deadline wrap; first NAK before wrap, next poll after wrap.
 raw=UINT32_MAX-4000;CHECK(hcd_edpt_xfer(0,2,0x81,buf,8));ch_id=channel_find_enabled(&regs,2,1,1);nak(ch_id);
 unsigned before=polls;raw=2000;task();CHECK(polls==before);raw=5000;task();CHECK(polls==before+1);
 CHECK(!usb_wait_failed());
 printf("DWC2: NAK retries >=8ms, SS/CS counted, SOF IRQ disabled, late/wrap/abort/close OK (%u polls, %u SS, %u CS)\n",polls,ss,cs);
 return 0;
}
