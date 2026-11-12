/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* The last-boot record lives in /OS/SAVES/KERNEL/BOOT.TXT (one short line), the Pi's name in
   /OS/NAME.TXT (written by "usb_kernel.py name"). Both are plain files on the card; every failure
   is only logged: a card that cannot be written never stops a boot. */
#include "kernel.h"
#include "assets.h"
#include "fatput.h"
#include "bootrec.h"
#include "render.h"
#include "usblog.h"
static uint32_t count;static enum boot_decision decision=BD_NONE;
static int slurp(const char *path,char *buf,uint32_t cap) {
 int h=asset_open(path,(uint32_t)strlen(path),0);if(h<0)return -1;
 int n=asset_read(h,buf,cap);asset_close(h);return n;
}
static void save(unsigned kbd) {
 char rec[BOOTINFO_REC_MAX+1];unsigned n=bootinfo_format(rec,count,decision,kbd);bootinfo_set_this(rec);
 rec[n++]='\n';
 int r=fatsave_put("KERNEL","BOOT.TXT",(const uint8_t *)rec,n);
 if(r) log_line("Boot record not saved");
}
void bootrec_boot(enum boot_decision d,bool new_boot) {
 char buf[64];int n=slurp("/OS/NAME.TXT",buf,16);
 char nm[BOOTINFO_NAME_MAX+1]="";
 if(n>0&&bootinfo_parse_name(buf,(uint32_t)n,nm)) {bootinfo_set_name(nm);render_set_tag(nm);}
 uint32_t prev=0;n=slurp("/OS/SAVES/KERNEL/BOOT.TXT",buf,sizeof buf-1);
 if(n>0) {bootinfo_set_prev(buf,(uint32_t)n);if(!bootinfo_parse_count(buf,(uint32_t)n,&prev))prev=0;}
 count=new_boot?prev+1:prev;decision=d;
 if(d!=BD_NONE) save(0);
}
void bootrec_keyboards(unsigned kbd) {if(decision!=BD_NONE) save(kbd);}
void bootrec_usblog(uint32_t attach,uint32_t remove) {
 static uint8_t out[USBLOG_MAX+80];uint32_t n=0;const char *t=usblog_text(&n);
 for(uint32_t i=0;i<n;i++) out[i]=(uint8_t)t[i];
 const char *a="events attach=";while(*a)out[n++]=(uint8_t)*a++;
 char d[11];unsigned k=0;uint32_t v=attach;do {d[k++]=(char)('0'+v%10);v/=10;} while(v);while(k)out[n++]=(uint8_t)d[--k];
 a=" remove=";while(*a)out[n++]=(uint8_t)*a++;
 v=remove;k=0;do {d[k++]=(char)('0'+v%10);v/=10;} while(v);while(k)out[n++]=(uint8_t)d[--k];
 out[n++]='\n';
 if(fatsave_put("KERNEL","USB.TXT",out,n)) log_line("USB log not saved");
}
int bootrec_usblog_part(unsigned i,char *o) {
 static char buf[USBLOG_MAX+80];int n=slurp("/OS/SAVES/KERNEL/USB.TXT",buf,sizeof buf);
 if(n<0) return -1;
 if(n==0) return 0;
 unsigned parts=((unsigned)n+USBLOG_PART-1)/USBLOG_PART;if(i>=parts) {o[0]=0;return (int)parts;}
 unsigned from=i*USBLOG_PART,to=from+USBLOG_PART;if(to>(unsigned)n)to=(unsigned)n;unsigned k=0;
 for(unsigned j=from;j<to;j++) o[k++]=buf[j]=='\n'?'|':buf[j];
 o[k]=0;return (int)parts;
}
