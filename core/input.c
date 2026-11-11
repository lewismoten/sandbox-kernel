/* SPDX-License-Identifier: MIT; original input event queue (input.h). */
#include "kernel.h"
#include "clock.h"
#include "input.h"
#define EAGAIN 11
#define ENODEV 19
#define EBUSY 16
#define EINVAL 22
static bool present,reading;
static uint32_t instance,head,count;
static uint64_t keys[INPUT_KEYS/64];
static int64_t axes[INPUT_AXES];
static struct input_event_rec queue[INPUT_QUEUE];
uint32_t input_dropped,input_activity;
static void clear_state(void) {memset(keys,0,sizeof keys);memset(axes,0,sizeof axes);head=count=0;reading=false;}
void input_init(void) {clear_state();present=true;instance=1;input_dropped=0;}
bool input_present(void) {return present;}
uint32_t input_instance(void) {return instance;}
void input_add(void) {if(present) return; clear_state();present=true;instance++;if(!instance) instance=1;}
void input_remove(void) {present=false;clear_state();}
static bool down(unsigned code) {return (keys[code/64]>>(code%64))&1u;}
static void put(uint64_t ns,uint16_t type,uint16_t code,int32_t value) {
 queue[(head+count)%INPUT_QUEUE]=(struct input_event_rec){ns,type,code,value};count++;
}
int input_frame(const struct input_change *c,unsigned n) {
 if(!present) return -ENODEV;
 if(!n||n>INPUT_FRAME_MAX) return -EINVAL;
 for(unsigned i=0;i<n;i++) {
  if(c[i].type==EV_KEY) {if(!c[i].code||c[i].code>=INPUT_KEYS||c[i].value<0||c[i].value>2) return -EINVAL;}
  else if(c[i].type==EV_ABS) {if(c[i].code>=INPUT_AXES||c[i].value<INPUT_AXIS_MIN||c[i].value>INPUT_AXIS_MAX) return -EINVAL;}
  else return -EINVAL;
 }
 /* Apply in order, keeping only real changes (and repeats of held keys). */
 struct input_change kept[INPUT_FRAME_MAX]; unsigned k=0;
 for(unsigned i=0;i<n;i++) {
  unsigned code=c[i].code; int32_t v=c[i].value;
  if(c[i].type==EV_KEY) {
   if(v==2) {if(down(code)) kept[k++]=c[i]; continue;}
   if(down(code)==(v==1)) continue;
   if(v) keys[code/64]|=1ull<<(code%64); else keys[code/64]&=~(1ull<<(code%64));
  } else {
   if(axes[code]==v) continue;
   axes[code]=v;
  }
  kept[k++]=c[i];
 }
 if(k) input_activity++;
 if(!k||!reading) return 0;
 uint64_t ns=clock_us()*1000u;
 if(count+k+1>INPUT_QUEUE) {head=count=0;put(ns,EV_SYN,SYN_DROPPED,0);input_dropped++;return 0;}
 for(unsigned i=0;i<k;i++) put(ns,kept[i].type,kept[i].code,kept[i].value);
 put(ns,EV_SYN,SYN_REPORT,0);
 return 0;
}
int input_open(uint32_t id) {
 if(!present||id!=instance) return -ENODEV;
 if(reading) return -EBUSY;
 reading=true;head=count=0;return 0;
}
void input_close(uint32_t id) {if(present&&id==instance) {reading=false;head=count=0;}}
int input_read(uint32_t id,struct input_event_rec *out,uint32_t max) {
 if(!present||id!=instance||!reading) return -ENODEV;
 if(!count) return -EAGAIN;
 uint32_t n=count<max?count:max;
 for(uint32_t i=0;i<n;i++) out[i]=queue[(head+i)%INPUT_QUEUE];
 head=(head+n)%INPUT_QUEUE;count-=n;return (int)n;
}
int input_state(uint32_t id,uint64_t k[INPUT_KEYS/64],int64_t a[INPUT_AXES]) {
 if(!present||id!=instance||!reading) return -ENODEV;
 memcpy(k,keys,sizeof keys);memcpy(a,axes,sizeof axes);head=count=0;return 0;
}
