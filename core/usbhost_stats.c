/* SPDX-License-Identifier: MIT; counters never log, allocate or read a clock in IRQ. */
#include "usbhost_work.h"
#include "usblog.h"
#define ENDPOINTS 16
static struct endpoint {
 uint8_t d,ep;bool active;
 uint32_t interval;
 volatile uint32_t submits,polls,ss,cs;
 uint32_t old_submits,old_polls,old_ss,old_cs;
} endpoints[ENDPOINTS];
static volatile uint32_t splits,irq_us,irq_max;
static uint32_t task_us,pass_max,overruns,passes;
static struct endpoint *find(uint8_t d,uint8_t ep) {
 for(unsigned i=0;i<ENDPOINTS;i++) if(endpoints[i].d==d&&endpoints[i].ep==ep)return &endpoints[i];
 return 0;
}
void usbhost_stats_open(uint8_t d,uint8_t ep,uint32_t interval) {
 struct endpoint *p=find(d,ep);
 if(!p) for(unsigned i=0;i<ENDPOINTS;i++) if(!endpoints[i].active) {p=&endpoints[i];*p=(struct endpoint){0};break;}
 if(p) {p->d=d;p->ep=ep;p->active=true;p->interval=interval;}
}
void usbhost_stats_close(uint8_t d,uint8_t ep) {
 for(unsigned i=0;i<ENDPOINTS;i++)if(endpoints[i].d==d&&(!ep||endpoints[i].ep==ep))endpoints[i].active=false;
}
void usbhost_stats_submit(uint8_t d,uint8_t ep) {struct endpoint *p=find(d,ep);if(p)p->submits++;}
void usbhost_stats_token(uint8_t d,uint8_t ep,bool interrupt,bool split,bool complete) {
 if(split)splits++;
 if(!interrupt)return;
 struct endpoint *p=find(d,ep);if(!p)return;
 if(!split||!complete)p->polls++;
 if(split) {if(complete)p->cs++;else p->ss++;}
}
void usbhost_stats_irq(uint32_t us) {irq_us+=us;if(us>irq_max)irq_max=us;}
void usbhost_stats_pass(uint32_t us) {task_us+=us;passes++;if(us>pass_max)pass_max=us;if(us>USBHOST_TASK_BUDGET_US)overruns++;}
/* All text work is foreground and outside the measured stack call. Lifetime
 * maxima and overruns survive the one-second windows. Explicit window length
 * makes late service distinguishable from a true one-second transfer rate. */
static void put(char **p,const char *s) {while(*s)*(*p)++=*s++;**p=0;}
static void dec(char **p,uint32_t n) {char digits[10];unsigned k=0;do{digits[k++]=(char)('0'+n%10);n/=10;}while(n);while(k)*(*p)++=digits[--k];**p=0;}
static void field(char **p,const char *s,uint32_t n) {put(p,s);dec(p,n);}
void log_line(const char *s);
void usbhost_stats_task(void) {
 static uint32_t since,status_at,old_splits,old_irq,old_task,old_passes;
 uint32_t now=usb_wait_now(),window=now-since;if(window<1000000u)return;since=now;
 bool status=(uint32_t)(now-status_at)>=5000000u;if(status)status_at=now;
 char snapshot[USBLOG_STATS_MAX],*out=snapshot,line[192],*p=line;
 uint32_t sp=splits,irq=irq_us,task=task_us,ps=passes;
 field(&p,"USB win_ms=",window/1000);field(&p," split=",sp-old_splits);field(&p," passes=",ps-old_passes);
 field(&p," task_us=",task-old_task);put(&p,"\n");
 uint32_t irq_delta=irq-old_irq;
 old_splits=sp;old_irq=irq;old_task=task;old_passes=ps;
 /* Split the summary to keep every line within USBLOG_LINE, even at saturation. */
 put(&out,line);if(status){p[-1]=0;log_line(line);}p=line;
 field(&p,"USB irq_us=",irq_delta);field(&p," max_irq=",irq_max);field(&p," max_pass=",pass_max);field(&p," over=",overruns);put(&p,"\n");
 put(&out,line);if(status){p[-1]=0;log_line(line);}
 for(unsigned i=0;i<ENDPOINTS;i++) {
  struct endpoint *e=&endpoints[i];if(!e->d)continue;
  uint32_t sub=e->submits,pol=e->polls,ss=e->ss,cs=e->cs;
  p=line;field(&p,"D",e->d);put(&p," ep");*p++="0123456789ABCDEF"[e->ep>>4];*p++="0123456789ABCDEF"[e->ep&15];*p=0;
  field(&p," submit=",sub-e->old_submits);field(&p," poll=",pol-e->old_polls);
  field(&p," ss=",ss-e->old_ss);field(&p," cs=",cs-e->old_cs);field(&p," us=",e->interval);
  put(&p,e->active?"\n":" closed\n");
  e->old_submits=sub;e->old_polls=pol;e->old_ss=ss;e->old_cs=cs;
  /* Keep whole lines; the reserved snapshot is bounded even at saturation. */
  if((unsigned)(out-snapshot)+(unsigned)(p-line)<sizeof(snapshot))put(&out,line);
  if(status){p[-1]=0;log_line(line);}
 }
 usblog_set_stats(snapshot);
}
