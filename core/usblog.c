/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "usblog.h"
static char buf[USBLOG_MAX+1];static uint32_t len,ver;static bool full;
void usblog_clear(void) {len=0;buf[0]=0;full=false;ver++;}
void usblog_add(const char *line) {
 if(full) return;
 uint32_t n=0;while(line[n]&&n<USBLOG_LINE-1) n++;
 if(len+n+1>USBLOG_MAX-12) { /* keep room for the closing line */
  const char *m="log full\n";for(const char *s=m;*s;s++) buf[len++]=*s;buf[len]=0;full=true;ver++;return;
 }
 for(uint32_t i=0;i<n;i++) buf[len++]=line[i]=='\n'||line[i]=='\r'?' ':line[i];
 buf[len++]='\n';buf[len]=0;ver++;
}
const char *usblog_text(uint32_t *bytes) {if(bytes)*bytes=len;return buf;}
uint32_t usblog_version(void) {return ver;}
static unsigned put(char *o,unsigned at,const char *s) {while(*s&&at<USBLOG_LINE-1)o[at++]=*s++;o[at]=0;return at;}
static unsigned dec(char *o,unsigned at,uint32_t v) {char t[11];unsigned k=0;do {t[k++]=(char)('0'+v%10);v/=10;} while(v);while(k&&at<USBLOG_LINE-1)o[at++]=t[--k];o[at]=0;return at;}
static unsigned hex(char *o,unsigned at,uint32_t v,unsigned digits) {for(unsigned i=0;i<digits&&at<USBLOG_LINE-1;i++)o[at++]="0123456789ABCDEF"[(v>>(4*(digits-1-i)))&15];o[at]=0;return at;}
void usblog_fmt_device(char *o,uint8_t daddr,uint8_t hub,uint8_t port,const char *speed,bool split,
                       uint16_t vid,uint16_t pid,uint8_t cls,uint8_t sub,uint8_t proto,uint8_t ep0) {
 unsigned a=0;o[0]=0;a=put(o,a,"D");a=dec(o,a,daddr);
 if(hub){a=put(o,a," hub");a=dec(o,a,hub);a=put(o,a," port");a=dec(o,a,port);} else a=put(o,a," otg-port");
 a=put(o,a," ");a=put(o,a,speed);if(split)a=put(o,a," split");
 a=put(o,a," ");a=hex(o,a,vid,4);a=put(o,a,":");a=hex(o,a,pid,4);
 a=put(o,a," class ");a=dec(o,a,cls);a=put(o,a,"/");a=dec(o,a,sub);a=put(o,a,"/");a=dec(o,a,proto);
 a=put(o,a," ep0 ");(void)dec(o,a,ep0);
}
void usblog_fmt_interface(char *o,uint8_t daddr,uint8_t num,uint8_t cls,uint8_t sub,uint8_t proto,uint8_t neps) {
 unsigned a=0;o[0]=0;a=put(o,a,"D");a=dec(o,a,daddr);a=put(o,a," I");a=dec(o,a,num);
 a=put(o,a," class ");a=dec(o,a,cls);a=put(o,a,"/");a=dec(o,a,sub);a=put(o,a,"/");a=dec(o,a,proto);
 a=put(o,a," eps ");(void)dec(o,a,neps);
}
void usblog_fmt_endpoint(char *o,uint8_t daddr,uint8_t num,uint8_t addr,uint8_t attr,uint16_t mps,uint8_t interval) {
 unsigned a=0;o[0]=0;a=put(o,a,"D");a=dec(o,a,daddr);a=put(o,a," I");a=dec(o,a,num);
 a=put(o,a," ep 0x");a=hex(o,a,addr,2);a=put(o,a,(attr&3)==3?" int":(attr&3)==2?" bulk":(attr&3)==1?" iso":" ctrl");
 a=put(o,a," mps ");a=dec(o,a,mps);a=put(o,a," every ");a=dec(o,a,interval);
}
void usblog_fmt_note(char *o,uint8_t daddr,uint8_t num,const char *text) {
 unsigned a=0;o[0]=0;a=put(o,a,"D");a=dec(o,a,daddr);
 if(num!=0xFF){a=put(o,a," I");a=dec(o,a,num);}
 a=put(o,a," ");(void)put(o,a,text);
}

void usblog_fmt_control(char *o,uint8_t d,uint8_t type,uint8_t req,uint16_t value,uint16_t index,uint16_t length,unsigned retry,unsigned delay_ms,const char *outcome) {
 unsigned a=0;o[0]=0;a=put(o,a,"D");a=dec(o,a,d);a=put(o,a," CTRL ");a=hex(o,a,type,2);a=put(o,a,"/");a=hex(o,a,req,2);
 a=put(o,a," v=");a=hex(o,a,value,4);a=put(o,a," i=");a=hex(o,a,index,4);a=put(o,a," len=");a=dec(o,a,length);
 a=put(o,a," ");a=put(o,a,outcome);a=put(o,a," retry=");a=dec(o,a,retry);
 if(delay_ms){a=put(o,a," wait=");a=dec(o,a,delay_ms);a=put(o,a,"ms");}
}
void usblog_fmt_port_retry(char *o,uint8_t hub,uint8_t port,unsigned retry,const char *outcome) {
 unsigned a=0;o[0]=0;a=put(o,a,"hub");a=dec(o,a,hub);a=put(o,a," port");a=dec(o,a,port);
 a=put(o,a," enum ");a=put(o,a,outcome);a=put(o,a," ");a=dec(o,a,retry);(void)put(o,a,"/2");
}
