/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "bootinfo.h"
static char prev[BOOTINFO_REC_MAX],cur[BOOTINFO_REC_MAX],nm[BOOTINFO_NAME_MAX+1];
const char *bootinfo_status(enum boot_decision d) {
 switch(d) {case BD_NO_ADAPTER:return "NO OTG ADAPTER";case BD_INSTALL_KBD:return "OTG: INSTALL KBD";case BD_KEYBOARD:return "KEYBOARD MODE";default:return 0;}
}
const char *bootinfo_decision_name(enum boot_decision d) {
 switch(d) {case BD_LINK:return "link";case BD_NO_ADAPTER:return "no-adapter";case BD_INSTALL_KBD:return "install-kbd";case BD_KEYBOARD:return "keyboard";default:return "none";}
}
static unsigned put(char *o,unsigned at,const char *s) {while(*s&&at<BOOTINFO_REC_MAX-1)o[at++]=*s++;return at;}
static bool reset_set;static uint32_t reset_rsts;
const char *bootinfo_reset_name(uint32_t r) {return r&0x1000u?"power-on":r&0x70u?"watchdog":r&0x700u?"software":"unknown";}
void bootinfo_set_reset(uint32_t r) {reset_set=true;reset_rsts=r;}
unsigned bootinfo_format(char *out,uint32_t n,enum boot_decision d,unsigned keyboards) {
 char dig[12];unsigned k=0,at=0;
 out[at++]='n';do {dig[k++]=(char)('0'+n%10);n/=10;} while(n&&k<11);while(k)out[at++]=dig[--k];
 out[at++]=',';at=put(out,at,bootinfo_decision_name(d));
 out[at++]=',';out[at++]='k';if(keyboards>9)keyboards=9;out[at++]=(char)('0'+keyboards);
 if(reset_set) {
  out[at++]=',';out[at++]='r';at=put(out,at,bootinfo_reset_name(reset_rsts));out[at++]=':';
  char hx[8];unsigned q=0;uint32_t v=reset_rsts&0xFFFFu;do {hx[q++]="0123456789abcdef"[v&15u];v>>=4;} while(v);while(q)out[at++]=hx[--q];
 }
 out[at]=0;return at;
}
bool bootinfo_parse_count(const char *t,uint32_t bytes,uint32_t *n) {
 if(bytes<2||t[0]!='n') return false;
 uint32_t v=0;unsigned i=1;
 for(;i<bytes&&t[i]>='0'&&t[i]<='9';i++) {if(v>429496729u) return false;v=v*10+(uint32_t)(t[i]-'0');}
 if(i==1) return false;
 *n=v;return true;
}
bool bootinfo_parse_name(const char *t,uint32_t bytes,char out[BOOTINFO_NAME_MAX+1]) {
 out[0]=0;uint32_t n=0;
 while(n<bytes&&t[n]!='\n'&&t[n]!='\r'&&t[n]!=0) n++;
 if(n<bytes&&n==0) return false;
 if(n==0||n>BOOTINFO_NAME_MAX) return false;
 for(uint32_t i=0;i<n;i++) {char c=t[i];if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-')) return false;}
 for(uint32_t i=0;i<n;i++) out[i]=t[i];
 out[n]=0;return true;
}
void bootinfo_set_prev(const char *t,uint32_t bytes) {
 unsigned i=0;while(i<bytes&&i<BOOTINFO_REC_MAX-1&&t[i]>' ') {prev[i]=t[i];i++;}prev[i]=0;
}
void bootinfo_set_this(const char *t) {unsigned i=0;while(t[i]&&i<BOOTINFO_REC_MAX-1) {cur[i]=t[i];i++;}cur[i]=0;}
void bootinfo_set_name(const char *s) {unsigned i=0;while(s[i]&&i<BOOTINFO_NAME_MAX) {nm[i]=s[i];i++;}nm[i]=0;}
const char *bootinfo_prev(void) {return prev[0]?prev:"none";}
const char *bootinfo_this(void) {return cur[0]?cur:"none";}
const char *bootinfo_name(void) {return nm[0]?nm:"-";}
