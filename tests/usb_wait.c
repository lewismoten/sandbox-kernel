/* SPDX-License-Identifier: MIT; actual DWC2 implementation, fake MMIO and time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/tusb_common.h"
#include "portable/synopsys/dwc2/dwc2_type.h"
static dwc2_regs_t regs;
#define DWC2_REG(_port) (&regs)
#include "portable/synopsys/dwc2/dwc2_common.c"
#ifdef USB_ROLE_HOST
#include "portable/synopsys/dwc2/hcd_dwc2.c"
void hcd_event_handler(hcd_event_t const *e,bool isr) {(void)e;(void)isr;}
bool tuh_bus_info_get(uint8_t d,tuh_bus_info_t *i) {(void)d;memset(i,0,sizeof(*i));return true;}
#else
#include "portable/synopsys/dwc2/dcd_dwc2.c"
void dcd_event_handler(dcd_event_t const *e,bool isr) {(void)e;(void)isr;}
void usbd_spin_lock(bool isr) {(void)isr;}
void usbd_spin_unlock(bool isr) {(void)isr;}
#endif
#include "usblog.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void log_line(const char *s) {(void)s;}
static enum usb_wait_kind target;
static uint32_t now;
static bool emulate;
void idle_wait(uint32_t us) {now+=us;}
static void put(volatile const uint32_t *p,uint32_t n) {*(volatile uint32_t *)(uintptr_t)p=n;}
uint32_t micros(void) {
 if(emulate) {
  if(target!=USB_WAIT_AHB) regs.grstctl|=GRSTCTL_AHBIDL;
  if(target==USB_WAIT_AHB&&(regs.grstctl&GRSTCTL_CSRST)) regs.grstctl&=~GRSTCTL_AHBIDL;
  if(target!=USB_WAIT_RESET) regs.grstctl&=~GRSTCTL_CSRST;
  if(target!=USB_WAIT_RESET_DONE) regs.grstctl|=GRSTCTL_CSRST_DONE;
  if(target!=USB_WAIT_TX_FLUSH) regs.grstctl&=~GRSTCTL_TXFFLSH;
  if(target!=USB_WAIT_RX_FLUSH) regs.grstctl&=~GRSTCTL_RXFFLSH;
  if(target==USB_WAIT_HOST_MODE) put(&regs.gintsts,0);
 }
 return now++;
}
int main(int argc,char **argv) {
 CHECK(argc==4);unsigned variant=(unsigned)atoi(argv[3]);target=(enum usb_wait_kind)atoi(argv[1]);now=(uint32_t)strtoul(argv[2],NULL,0);
 if(variant) {
  for(unsigned i=0;i<200;i++) usblog_add("pre-timeout enumeration log filler ..............................................");
 }
 uint32_t start=now,bound=USB_WAIT_REG_US+8;bool ok=false;
 regs.gintmsk=0xffffffffu;regs.gahbcfg=GAHBCFG_GINT;
 put(&regs.gsnpsid, target==USB_WAIT_RESET_DONE?DWC2_CORE_REV_4_20a:DWC2_CORE_REV_2_80a);
 switch(target) {
 case USB_WAIT_AHB:
  if(variant) {emulate=true;regs.grstctl=GRSTCTL_AHBIDL;}
  ok=dwc2_core_init(0,false,false);break;
 case USB_WAIT_RESET: case USB_WAIT_RESET_DONE:
  regs.grstctl=GRSTCTL_AHBIDL;ok=reset_core(&regs);break;
 case USB_WAIT_TX_FLUSH:
  emulate=variant!=0;ok=variant?dwc2_core_init(0,false,false):dfifo_flush_tx(&regs,0x10);break;
 case USB_WAIT_RX_FLUSH:
  emulate=variant!=0;ok=variant?dwc2_core_init(0,false,false):dfifo_flush_rx(&regs);break;
 case USB_WAIT_RX_IRQ:
  put(&regs.gintsts,GINTSTS_RXFLVL);regs.gintmsk=GINTSTS_RXFLVL;
  bound=USB_WAIT_IRQ_US+8;
#ifdef USB_ROLE_HOST
  hcd_int_handler(0,true);
#else
  dcd_int_handler(0);
#endif
  break;
#ifdef USB_ROLE_HOST
 case USB_WAIT_HOST_MODE:
  emulate=true;bound=USB_WAIT_MODE_US+24;ok=hcd_init(0,NULL);break;
 case USB_WAIT_REQUEST: {
  dwc2_channel_t *ch=&regs.channel[0];
  dwc2_channel_char_t hc={.ep_type=variant&1?HCCHAR_EPTYPE_INTERRUPT:HCCHAR_EPTYPE_BULK};ch->hcchar=hc.value;
  ok=variant&2?channel_disable(&regs,ch):channel_send_in_token(&regs,ch);break;
 }
#else
 case USB_WAIT_IN_NAK: case USB_WAIT_IN_DISABLE:
  regs.epin[1].diepctl=DIEPCTL_EPENA;
  if(target==USB_WAIT_IN_DISABLE)regs.epin[1].diepint=DIEPINT_INEPNE;
  ok=edpt_disable(0,0x81,true);break;
 case USB_WAIT_OUT_NAK: case USB_WAIT_OUT_DISABLE:
  regs.epout[1].doepctl=DOEPCTL_EPENA;
  if(target==USB_WAIT_OUT_DISABLE)put(&regs.gintsts,GINTSTS_BOUTNAKEFF_Msk);
  ok=edpt_disable(0,1,true);break;
#endif
 case USB_WAIT_SEMAPHORE: {
  osal_semaphore_def_t sem={0};ok=osal_semaphore_wait(&sem,UINT32_MAX);break;
 }
 case USB_WAIT_DELAY: tusb_time_delay_ms_api(10000);break;
 default: CHECK(false);
 }
 CHECK(!ok);CHECK(usb_wait_failed());CHECK(usb_wait_count(target)==1);
 CHECK((uint32_t)(now-start)<=bound);
 if(target<USB_WAIT_SEMAPHORE) {CHECK(!regs.gintmsk);CHECK(!(regs.gahbcfg&GAHBCFG_GINT));}
 usb_wait_report();uint32_t bytes;const char *log=usblog_text(&bytes);
 CHECK(bytes>0);CHECK(strstr(log,"DWC2 timeout "));CHECK(strstr(log,"count=1"));
 /* Model the next kernel iterations: a terminal USB fault must neither spin
  * again nor consume time, and the unrelated loop keeps making progress. */
 unsigned ticks=0;uint32_t stopped=now;
 for(unsigned i=0;i<1000;i++) {
#ifdef USB_ROLE_HOST
  hcd_int_handler(0,true);
  CHECK(!hcd_edpt_xfer(0,1,0x81,NULL,0));
#else
  dcd_int_handler(0);
  CHECK(!dcd_edpt_xfer(0,0x81,NULL,0,false));
#endif
  ticks++;
 }
 CHECK(ticks==1000);CHECK(now==stopped);CHECK(usb_wait_count(target)==1);
 return 0;
}
