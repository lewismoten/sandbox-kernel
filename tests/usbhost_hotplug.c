/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* Hot-plug sequences, end to end: the real patched TinyUSB host + hub + HID drivers, the real kernel
 * glue (callbacks and usb_task copied from drivers/usb_host.c by the runner) and core/usbkbd.c +
 * core/input.c, over a scripted fake bus. The bus is a model of a root port, a four-port hub
 * (1A40:0101-like) and boot keyboards: it answers every control request the stack sends (descriptors,
 * SET_ADDRESS, SET_CONFIGURATION, the hub's port power, reset and status requests, HID class setup),
 * delivers the hub's status-change and the keyboards' interrupt reports, and drops everything behind a
 * pulled hub. Nothing is shortcut in the stack: it debounces, resets, addresses and configures each
 * plug as at boot. Time is virtual (microseconds); each loop turn is one main-loop pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbh.c"
#include "hub.c"
#include "usbkbd.h"
#include "input.h"
#include "usblog.h"
static int step;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d (step %d): %s\n",__LINE__,step,#x);exit(1);}}while(0)
#define KEY_A 30
#define KEY_B 48

static uint32_t raw;
static unsigned max_pass,passes,ready_lines,removed_lines,addr_requests,double_arms;
uint32_t micros(void) {return raw;}
uint32_t usb_wait_now(void) {return raw++;}
bool usb_wait_failed(void) {return false;}
uint32_t tusb_time_millis_api(void) {return raw/1000;}
void tusb_time_delay_ms_api(uint32_t ms) {raw+=ms*1000;}
uint64_t clock_us(void) {static uint32_t last;static uint64_t time;time+=(uint32_t)(raw-last);last=raw;return time;}
void log_line(const char *s) {
 if(!strcmp(s,"keyboard ready"))ready_lines++;
 else if(!strcmp(s,"keyboard removed")||!strcmp(s,"keyboard poll failed"))removed_lines++;
}
void power_input(uint64_t time) {(void)time;}
void hcd_int_handler(uint8_t p,bool isr) {(void)p;(void)isr;}
void hcd_int_enable(uint8_t p) {(void)p;}
void hcd_int_disable(uint8_t p) {(void)p;}
bool hcd_init(uint8_t p,const tusb_rhport_init_t *i) {(void)p;(void)i;return true;}

/* ---- the bus model ---- */
enum {HUB,KB_FS,KB_LS,KB_ROOT,HUB2,KB2,NODES}; /* HUB2: a full-speed hub on HUB's port 3; KB2: a two-interface keyboard on its port 1 */
typedef struct {
 bool is_hub,plugged,armed,rep_pending;int parent,port;tusb_speed_t speed;uint8_t addr;
 const uint8_t *cfg;uint16_t cfglen,vid,pid;
 uint16_t ps[5],pc[5];bool pwr[5];int child[5];            /* hub ports 1..4 */
 uint8_t *ibuf;uint16_t ilen;uint8_t rep[8];
} node_t;
static node_t node[NODES];
static node_t *zero;                                         /* the device that answers at address 0 */
static bool open_ep[16][256];
static const uint8_t hub_cfg[]={9,2,25,0,1,1,0,0xE0,50, 9,4,0,0,1,9,0,0,0, 7,5,0x81,3,1,0,12};
static const uint8_t kb_cfg[]={9,2,34,0,1,1,0,0x80,50, 9,4,0,0,1,3,1,1,0, 9,0x21,0x11,1,0,1,0x22,63,0, 7,5,0x81,3,8,0,10};
/* The keyboard-with-a-hub shape: a boot keyboard (ep 0x81) and a second HID interface (mouse-like, ep 0x82 every 1 ms) that is never polled. */
static const uint8_t kb2_cfg[]={9,2,59,0,2,1,0,0x80,50, 9,4,0,0,1,3,1,1,0, 9,0x21,0x11,1,0,1,0x22,63,0, 7,5,0x81,3,8,0,10,
                                9,4,1,0,1,3,1,2,0, 9,0x21,0x11,1,0,1,0x22,63,0, 7,5,0x82,3,8,0,1};
static bool reach(int i) {return node[i].plugged&&(node[i].parent<0||reach(node[i].parent));}
static node_t *by_addr(uint8_t a) {if(!a)return zero;for(int i=0;i<NODES;i++)if(node[i].plugged&&node[i].addr==a)return &node[i];return 0;}
static node_t *root_dev(void) {for(int i=0;i<NODES;i++)if(node[i].plugged&&node[i].parent<0)return &node[i];return 0;}
static uint16_t speed_bits(const node_t *c) {return c->speed==TUSB_SPEED_LOW?0x200:c->speed==TUSB_SPEED_HIGH?0x400:0;}
static void forget(node_t *n) {n->addr=0;n->armed=false;n->rep_pending=false;if(zero==n)zero=0;if(n->is_hub)for(int p=1;p<=4;p++)if(n->child[p]>=0)forget(&node[n->child[p]]);} /* a hub's whole subtree */
static void hub_power_cycle(node_t *h) {
 for(int p=1;p<=4;p++) {h->pwr[p]=false;h->ps[p]=h->pc[p]=0;if(h->child[p]>=0)forget(&node[h->child[p]]);}
}
static void set_up(int i,int parent,int port,tusb_speed_t s,const uint8_t *cfg,uint16_t len,bool hub,uint16_t vid,uint16_t pid) {
 node[i]=(node_t){.is_hub=hub,.parent=parent,.port=port,.speed=s,.cfg=cfg,.cfglen=len,.vid=vid,.pid=pid};
 for(int p=0;p<5;p++)node[i].child[p]=-1;
 if(parent>=0)node[parent].child[port]=i;
}
static void complete(uint8_t d,uint8_t ep,uint32_t n,xfer_result_t r) {hcd_event_xfer_complete(d,ep,n,r,false);}
static void plug(int i) {
 node_t *n=&node[i];n->plugged=true;forget(n);
 if(n->is_hub)hub_power_cycle(n);
 if(n->parent<0) {hcd_event_device_attach(0,false);return;}
 node_t *h=&node[n->parent];
 if(reach(n->parent)&&h->pwr[n->port]) {h->ps[n->port]|=0x103|speed_bits(n);h->ps[n->port]&=(uint16_t)~2u;h->pc[n->port]|=1;}
}
static void unplug(int i) {
 node_t *n=&node[i];n->plugged=false;forget(n);
 if(n->is_hub)for(int p=1;p<=4;p++)if(n->child[p]>=0)forget(&node[n->child[p]]);
 if(n->parent<0) {hcd_event_device_remove(0,false);return;}
 node_t *h=&node[n->parent];
 if(reach(n->parent)) {h->ps[n->port]&=0x100;h->pc[n->port]|=1;}
}
/* Root port: the stack's debounce, reset and speed query. */
bool hcd_port_connect_status(uint8_t p) {(void)p;return root_dev()!=0;}
tusb_speed_t hcd_port_speed_get(uint8_t p) {(void)p;node_t *d=root_dev();return d&&d->is_hub?TUSB_SPEED_HIGH:d?d->speed:TUSB_SPEED_FULL;}
void hcd_port_reset(uint8_t p) {(void)p;zero=root_dev();if(zero){zero->addr=0;if(zero->is_hub)hub_power_cycle(zero);}}
void hcd_port_reset_end(uint8_t p) {(void)p;}
bool hcd_edpt_open(uint8_t p,uint8_t d,const tusb_desc_endpoint_t *e) {(void)p;CHECK(d<16);open_ep[d][e->bEndpointAddress]=true;return true;}
bool hcd_edpt_close(uint8_t p,uint8_t d,uint8_t e) {(void)p;open_ep[d][e]=false;node_t *n=by_addr(d);if(n&&d&&e==0x81)n->armed=false;return true;}
void hcd_device_close(uint8_t p,uint8_t d) {(void)p;memset(open_ep[d],0,sizeof open_ep[d]);node_t *n=d?by_addr(d):0;if(n)n->armed=false;}
bool hcd_edpt_abort_xfer(uint8_t p,uint8_t d,uint8_t e) {(void)p;node_t *n=by_addr(d);if(n&&d&&e==0x81)n->armed=false;return true;}
bool hcd_edpt_clear_stall(uint8_t p,uint8_t d,uint8_t e) {(void)p;(void)d;(void)e;return true;}

/* Control requests: the reply is computed at the setup stage; data and status stages follow. */
static struct {node_t *t;tusb_control_request_t r;uint8_t reply[64];uint16_t len;bool fail,stall;} c;
static void std_request(node_t *t,const tusb_control_request_t *r) {
 uint8_t type=(uint8_t)(r->wValue>>8);
 switch(r->bRequest) {
  case 6: /* GET_DESCRIPTOR */
   if(type==1) {
    uint8_t dd[18]={18,1,0,2,t->is_hub?9:0,0,t->is_hub?1:0,64,(uint8_t)t->vid,(uint8_t)(t->vid>>8),(uint8_t)t->pid,(uint8_t)(t->pid>>8),0,1,0,0,0,1};
    memcpy(c.reply,dd,18);c.len=18;
   } else if(type==2) {memcpy(c.reply,t->cfg,t->cfglen);c.len=t->cfglen;}
   else if(type==0x22) {memset(c.reply,0,63);c.len=63;}
   else c.stall=true; /* the keyboards refuse the string requests, as the logs show */
   break;
  case 5: /* SET_ADDRESS */
   CHECK(t==zero&&!t->addr);for(int i=0;i<NODES;i++)CHECK(!(node[i].plugged&&node[i].addr==r->wValue));
   t->addr=(uint8_t)r->wValue;zero=0;addr_requests++;break;
  default: break; /* SET_CONFIGURATION, SET_INTERFACE: nothing to model */
 }
}
static void hub_request(node_t *h,const tusb_control_request_t *r) {
 unsigned p=r->wIndex;
 if(r->bmRequestType==0xA0&&r->bRequest==6) { /* hub descriptor */
  uint8_t hd[9]={9,0x29,4,0,0,50,0,0,0xFF};memcpy(c.reply,hd,9);c.len=9;
 } else if(r->bmRequestType==0xA0&&r->bRequest==0) {memset(c.reply,0,4);c.len=4;}
 else if(r->bmRequestType==0xA3&&r->bRequest==0) {
  CHECK(p>=1&&p<=4);c.reply[0]=(uint8_t)h->ps[p];c.reply[1]=(uint8_t)(h->ps[p]>>8);c.reply[2]=(uint8_t)h->pc[p];c.reply[3]=(uint8_t)(h->pc[p]>>8);c.len=4;
 } else if(r->bmRequestType==0x23&&r->bRequest==3) { /* SET_FEATURE on a port */
  CHECK(p>=1&&p<=4);
  if(r->wValue==8) {
   h->pwr[p]=true;h->ps[p]|=0x100;
   if(h->child[p]>=0&&node[h->child[p]].plugged) {node_t *k=&node[h->child[p]];h->ps[p]|=1|speed_bits(k);h->pc[p]|=1;}
  } else if(r->wValue==4) { /* PORT_RESET: done at once; the device then answers at address 0 */
   h->ps[p]|=2;h->pc[p]|=0x10;
   if(h->child[p]>=0&&node[h->child[p]].plugged) {zero=&node[h->child[p]];zero->addr=0;}
  }
 } else if(r->bmRequestType==0x23&&r->bRequest==1) { /* CLEAR_FEATURE on a port */
  CHECK(p>=1&&p<=4);
  if(r->wValue==16)h->pc[p]&=(uint16_t)~1u;else if(r->wValue==17)h->pc[p]&=(uint16_t)~2u;else if(r->wValue==20)h->pc[p]&=(uint16_t)~0x10u;
 }
}
bool hcd_setup_send(uint8_t p,uint8_t d,const uint8_t s[8]) {
 (void)p;memset(&c,0,sizeof c);memcpy(&c.r,s,8);raw+=60;
 c.t=by_addr(d);if(!c.t||(!d&&!zero)||!reach((int)(c.t-node)))c.fail=true;
 else if((c.r.bmRequestType&0x60)==0) std_request(c.t,&c.r);
 else if((c.r.bmRequestType&0x60)==0x20&&c.t->is_hub) hub_request(c.t,&c.r);
 /* HID class requests (SET_IDLE, SET_PROTOCOL, GET_PROTOCOL) need no model. */
 complete(d,0,8,c.fail?XFER_RESULT_FAILED:c.stall?XFER_RESULT_STALLED:XFER_RESULT_SUCCESS);
 return true;
}
bool hcd_edpt_xfer(uint8_t p,uint8_t d,uint8_t e,uint8_t *b,uint16_t n) {
 (void)p;raw+=120;
 if(!(e&0x7f)) { /* control data or status stage */
  if(c.fail||!c.t||!reach((int)(c.t-node))) {complete(d,e,0,XFER_RESULT_FAILED);return true;}
  uint16_t k=0;if(n&&(e&0x80)) {k=n<c.len?n:c.len;memcpy(b,c.reply,k);}
  complete(d,e,k,XFER_RESULT_SUCCESS);return true;
 }
 CHECK(e==0x81);node_t *t=by_addr(d);
 if(!t||!d||!open_ep[d][e]) return false; /* an endpoint that is not open (a device that just left) refuses, as the DWC2 driver does */
 if(t->armed)double_arms++;
 t->armed=true;t->ibuf=b;t->ilen=n;return true;
}
/* What the devices put on their interrupt pipes. */
static void pump(void) {
 for(int i=0;i<NODES;i++) {
  node_t *t=&node[i];if(!t->armed||!reach(i))continue;
  if(t->is_hub) {
   uint8_t bm=0;for(int p=1;p<=4;p++)if(t->pc[p])bm|=(uint8_t)(1u<<p);
   if(bm) {t->ibuf[0]=bm;t->armed=false;complete(t->addr,0x81,1,XFER_RESULT_SUCCESS);}
  } else if(t->rep_pending) {
   t->rep_pending=false;memcpy(t->ibuf,t->rep,8);t->armed=false;complete(t->addr,0x81,8,XFER_RESULT_SUCCESS);
  }
 }
}
static void key(int i,uint8_t usage) {memset(node[i].rep,0,8);node[i].rep[2]=usage;node[i].rep_pending=true;}

void usb_task(void); /* the real one, from drivers/usb_host.c (host-driver.c) */

/* ---- driving it ---- */
static void pass(void) {
 pump();
 uint32_t begin=raw;usb_task();uint32_t elapsed=raw-begin;passes++;
 CHECK(elapsed<=USBHOST_TASK_BUDGET_US); /* a pass never blocks past its budget */
 if(elapsed>max_pass)max_pass=elapsed;
}
static void run_ms(unsigned ms) {uint32_t end=raw+ms*1000u;while((int32_t)(raw-end)<0) {pass();raw+=250;}}
static bool held(unsigned code) {
 uint64_t k[INPUT_KEYS/64]={0};int64_t a[INPUT_AXES];
 if(!input_present())return false;
 uint32_t id=input_instance();input_close(id);CHECK(input_open(id)==0);
 CHECK(!input_state(id,k,a));return (k[code/64]>>(code%64))&1u;
}
static unsigned open_count(void) {unsigned n=0;for(unsigned d=0;d<16;d++)for(unsigned e=0;e<256;e++)n+=open_ep[d][e];return n;}
static unsigned device_slots(void) {unsigned n=0;for(unsigned a=1;a<=TOTAL_DEVICES;a++)if(get_device((uint8_t)a)->connected)n++;return n;}
static unsigned hid_slots(void) {unsigned n=0;for(unsigned a=1;a<=TOTAL_DEVICES;a++)for(unsigned i=0;i<CFG_TUH_HID;i++)if(tuh_hid_mounted((uint8_t)a,(uint8_t)i))n++;return n;}
static bool bus_idle(void) {
 return _usbh_data.enumerating_daddr==TUSB_INDEX_INVALID_8&&_usbh_data.ctrl_xfer_info.stage==CONTROL_STAGE_IDLE&&!_usbh_data.call_after.func&&
        !_usbh_data.attach_debouncing_bm&&!root_retry.retry_port;
}
static bool wait_ready(unsigned want,unsigned ms) {
 for(unsigned t=0;t<ms;t+=5) {if(usbkbd_count()==want&&(!want||bus_idle()))return true;run_ms(5);}
 return false;
}
/* Everything the stack holds for devices must be back to nothing once all are gone. */
static void check_empty(void) {
 CHECK(usbkbd_count()==0&&!input_present());CHECK(!strcmp(usbkbd_status(),"HOST: no device"));
 CHECK(device_slots()==0);CHECK(hid_slots()==0);CHECK(open_count()==0);CHECK(bus_idle());
 for(int i=0;i<NODES;i++)CHECK(!node[i].armed);
}
static void reports(int i,uint8_t usage,unsigned code) { /* a key down is seen, a key up too */
 key(i,usage);run_ms(60);CHECK(held(code));
 key(i,0);run_ms(60);CHECK(!held(code));
}
static unsigned baseline_open,baseline_devs;

int main(void) {
 input_init();usbkbd_init();input_remove(); /* input_init leaves the synthetic source present; a real boot has no keyboard yet */
 tusb_rhport_init_t init={.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH};CHECK(tusb_init(0,&init));
 set_up(HUB,-1,0,TUSB_SPEED_HIGH,hub_cfg,sizeof hub_cfg,true,0x1a40,0x0101);
 set_up(KB_FS,HUB,1,TUSB_SPEED_FULL,kb_cfg,sizeof kb_cfg,false,0x1997,0x2433);
 set_up(KB_LS,HUB,2,TUSB_SPEED_LOW,kb_cfg,sizeof kb_cfg,false,0x1a2c,0x2124);
 set_up(KB_ROOT,-1,0,TUSB_SPEED_FULL,kb_cfg,sizeof kb_cfg,false,0x045e,0x00db);
 set_up(HUB2,HUB,3,TUSB_SPEED_FULL,hub_cfg,sizeof hub_cfg,true,0x1234,0x0002);
 set_up(KB2,HUB2,1,TUSB_SPEED_FULL,kb2_cfg,sizeof kb2_cfg,false,0x1234,0x0003);
 run_ms(100);step=__LINE__;check_empty();

 /* 1. Hub with two keyboards (one full speed, one low speed behind the hub's translator), pulled, replugged. */
 node[HUB].plugged=false;node[KB_FS].plugged=true;node[KB_LS].plugged=true; /* both already on the hub's ports */
 plug(HUB);
 CHECK(wait_ready(2,4000));
 {const char *t=usblog_text(0);CHECK(strstr(t,"LS split")&&strstr(t,"FS split"));}
 reports(KB_FS,4,KEY_A);reports(KB_LS,5,KEY_B);
 key(KB_FS,4);run_ms(60);CHECK(held(KEY_A));            /* a key is held while the hub is pulled */
 unsigned removed=removed_lines;unplug(HUB);
 run_ms(30);                                             /* the stack sees the removal within a few passes */
 CHECK(usbkbd_count()==0&&!input_present()&&removed_lines==removed+2&&!held(KEY_A)); /* both released, held key cleared */
 run_ms(500);step=__LINE__;check_empty();
 plug(HUB);CHECK(wait_ready(2,4000));
 CHECK(!held(KEY_A));                                    /* the old held key did not come back */
 reports(KB_FS,4,KEY_A);reports(KB_LS,5,KEY_B);
 unplug(HUB);run_ms(500);step=__LINE__;check_empty();
 baseline_open=open_count();baseline_devs=device_slots();CHECK(baseline_open==0&&baseline_devs==0);

 /* 2. A keyboard in the root port: unplug, replug. */
 plug(KB_ROOT);CHECK(wait_ready(1,3000));reports(KB_ROOT,4,KEY_A);
 key(KB_ROOT,4);run_ms(60);CHECK(held(KEY_A));
 unplug(KB_ROOT);run_ms(30);CHECK(usbkbd_count()==0&&!held(KEY_A));run_ms(500);step=__LINE__;check_empty();
 plug(KB_ROOT);CHECK(wait_ready(1,3000));reports(KB_ROOT,4,KEY_A);
 unplug(KB_ROOT);run_ms(500);step=__LINE__;check_empty();

 /* 3. Contact bounce on the root port: five toggles inside the debounce window. */
 unsigned before=addr_requests;
 for(int k=0;k<5;k++) {if(k&1)unplug(KB_ROOT);else plug(KB_ROOT);run_ms(10);}
 CHECK(root_dev()!=0);CHECK(wait_ready(1,3000));
 CHECK(addr_requests==before+1);                         /* settled on the last state, one enumeration */
 reports(KB_ROOT,4,KEY_A);
 /* a mounted keyboard that bounces and ends unplugged leaves nothing behind, and later plugs work */
 before=addr_requests;
 for(int k=0;k<5;k++) {if(k&1)plug(KB_ROOT);else unplug(KB_ROOT);run_ms(10);}
 CHECK(root_dev()==0);run_ms(3000);step=__LINE__;check_empty();CHECK(addr_requests==before);
 plug(KB_ROOT);CHECK(wait_ready(1,3000));reports(KB_ROOT,4,KEY_A);
 unplug(KB_ROOT);run_ms(500);step=__LINE__;check_empty();

 /* 4. One keyboard behind the hub is pulled and replugged while the other keeps reporting. */
 plug(HUB);CHECK(wait_ready(2,4000));
 unsigned ready=ready_lines;
 unplug(KB_FS);
 for(int k=0;k<20;k++) {key(KB_LS,5);run_ms(30);CHECK(held(KEY_B));key(KB_LS,0);run_ms(30);CHECK(!held(KEY_B));} /* the other keeps reporting throughout */
 CHECK(usbkbd_count()==1);
 plug(KB_FS);CHECK(wait_ready(2,4000));CHECK(ready_lines==ready+1);
 reports(KB_FS,4,KEY_A);reports(KB_LS,5,KEY_B);
 unplug(KB_LS);run_ms(500);CHECK(usbkbd_count()==1);reports(KB_FS,4,KEY_A);
 plug(KB_LS);CHECK(wait_ready(2,4000));reports(KB_LS,5,KEY_B);

 /* 20 plug cycles of everything: nothing leaks. */
 for(int cycle=0;cycle<20;cycle++) {
  if(cycle%3==0) {unplug(HUB);run_ms(400);step=__LINE__;check_empty();plug(HUB);}
  else if(cycle%3==1) {unplug(KB_FS);run_ms(300);plug(KB_FS);}
  else {unplug(KB_LS);run_ms(300);plug(KB_LS);}
  CHECK(wait_ready(2,4000));
  CHECK(device_slots()==3&&hid_slots()==2);   /* hub + two keyboards, no extras */
 }
 reports(KB_FS,4,KEY_A);reports(KB_LS,5,KEY_B);
 unplug(HUB);run_ms(500);step=__LINE__;check_empty();
 /* Pulled at any moment of its own enumeration (a wiggling connector): the stack lets go, then a plug works. */
 for(unsigned at=0;at<1500;at+=7) {
  plug(HUB);run_ms(at);unplug(HUB);run_ms(2500);step=__LINE__;check_empty();
  plug(HUB);run_ms(at);unplug(KB_FS);run_ms(40);plug(KB_FS);CHECK(wait_ready(2,4000));unplug(HUB);run_ms(2500);step=__LINE__;check_empty();
 }
 /* The same for a keyboard in the root port, and for a keyboard on a hub that stays plugged. */
 for(unsigned at=0;at<1000;at+=7) {plug(KB_ROOT);run_ms(at);unplug(KB_ROOT);run_ms(2500);step=__LINE__;check_empty();}
 plug(HUB);CHECK(wait_ready(2,4000));
 for(unsigned at=0;at<800;at+=7) {
  unplug(KB_FS);run_ms(200);plug(KB_FS);run_ms(at);unplug(KB_FS);run_ms(1500);
  CHECK(usbkbd_count()==1&&bus_idle());reports(KB_LS,5,KEY_B);
 }
 plug(KB_FS);CHECK(wait_ready(2,4000));reports(KB_FS,4,KEY_A);reports(KB_LS,5,KEY_B);unplug(HUB);run_ms(500);step=__LINE__;check_empty();
 for(int cycle=0;cycle<20;cycle++) {plug(KB_ROOT);CHECK(wait_ready(1,3000));unplug(KB_ROOT);run_ms(300);step=__LINE__;check_empty();}
 /* 5. A hub behind a hub: HUB (high speed) -> HUB2 (full speed) -> KB2 (two interfaces). The keyboard works; pulling
  * the middle hub, or the outer one, at any moment of any transfer and plugging it back 50 times leaves nothing behind. */
 node[KB_FS].plugged=false;node[KB_LS].plugged=false;node[HUB2].plugged=true;node[KB2].plugged=true;
 plug(HUB);CHECK(wait_ready(1,6000));
 CHECK(device_slots()==3&&hid_slots()==1);                /* two hubs and one keyboard; its second interface has no slot */
 {const char *t=usblog_text(0);CHECK(strstr(t,"FS split"));CHECK(!strstr(t,"HID slots full"));}
 reports(KB2,4,KEY_A);
 for(int cycle=0;cycle<50;cycle++) {
  key(KB2,4);run_ms(20+(unsigned)(cycle*13)%90);          /* a key is down, a transfer is in flight */
  if(cycle%2==0) unplug(HUB2); else unplug(HUB);
  run_ms(30+(unsigned)(cycle*7)%200);
  if(cycle%5==0) {run_ms(2500);step=__LINE__;CHECK(usbkbd_count()==0&&!held(KEY_A));}
  if(cycle%2==0) plug(HUB2); else plug(HUB);
  if(cycle%7==3) {run_ms(10+(unsigned)cycle*11);if(cycle%2==0)unplug(HUB2);else unplug(HUB);run_ms(1500);if(cycle%2==0)plug(HUB2);else plug(HUB);} /* pulled again mid-enumeration */
  CHECK(wait_ready(1,6000));
  CHECK(device_slots()==3&&hid_slots()==1);
  CHECK(!held(KEY_A));
  reports(KB2,4,KEY_A);
 }
 unplug(HUB);run_ms(1000);step=__LINE__;check_empty();
 /* A hub that reports no ports is skipped with a log line and the bus stays usable. */
 plug(HUB);CHECK(wait_ready(1,6000));unplug(HUB);run_ms(1000);step=__LINE__;check_empty();
 CHECK(!double_arms);
 printf("hot-plug: %u passes, max pass %u/%u us, %u keyboards mounted in all, no leaks after 40 cycles\n",passes,max_pass,USBHOST_TASK_BUDGET_US,ready_lines);
 return 0;
}
