/* SPDX-License-Identifier: MIT; M1 boot probes for the runtime: read-only
 * asset checks over USB/UART logs. Reads only; changes no card sector. */
#include "kernel.h"
#include "assets.h"
static void append(char **p,const char *s) {while(*s)*(*p)++=*s++; **p=0;}
static void append_n(char **p,uint32_t n) {char t[12];number(t,n);append(p,t);}
static void append_e(char **p,int e) {if(e<0){append(p,"-");append_n(p,(uint32_t)-e);}else append_n(p,(uint32_t)e);}
static uint8_t chunk[4096];
/* M1 results kept for a reader that connects after boot: the Mac side can
   miss lines sent before it opened the port. */
static char notes[64][128]; static unsigned note_count;
void boot_note(const char *s) {
 log_line(s);
 if(note_count<64) {unsigned i=0;for(;s[i]&&i<127;i++)notes[note_count][i]=s[i];notes[note_count++][i]=0;}
}
void boot_summary(void) {for(unsigned i=0;i<note_count;i++) {char t[136],*p=t;append(&p,"BOOT ");append(&p,notes[i]);log_line(t);}}
void asset_probe(void) {
 char line[80],*p=line;
 int r=asset_mount();
 if(r) {append(&p,"ASSET mount ");append_e(&p,r);append(&p," ");append(&p,asset_fail);boot_note(line);return;}
 int d=asset_open("/",1,0); unsigned count=0;
 if(d<0) {append(&p,"ASSET root ");append_e(&p,d);append(&p," ");append(&p,asset_fail);boot_note(line);return;}
 struct asset_dirent e;
 while((r=asset_readdir(d,&e))==1) {
  if(++count<=12) {p=line;append(&p,"ASSET / ");append(&p,e.name);append(&p,e.is_dir?" dir":" ");if(!e.is_dir)append_n(&p,e.size);log_line(line);}
 }
 asset_close(d);
 p=line;append(&p,"ASSET root entries ");append_n(&p,count);if(r){append(&p," error ");append_e(&p,r);append(&p," ");append(&p,asset_fail);}boot_note(line);
 /* Whole-file read in 4 KiB chunks; the CRC compares with the host copy. */
 int f=asset_open("/mit.txt",8,0); p=line;
 if(f<0) {append(&p,"ASSET /MIT.TXT ");append_e(&p,f);append(&p," ");append(&p,asset_fail);boot_note(line);}
 else {
  uint32_t total=0,crc=~0u; int n;
  while((n=asset_read(f,chunk,sizeof(chunk)))>0) {
   total+=(uint32_t)n;
   for(int i=0;i<n;i++) {crc^=chunk[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^((0u-(crc&1))&0xedb88320u);}
  }
  asset_close(f);
  append(&p,"ASSET /MIT.TXT ");append_n(&p,total);append(&p," bytes crc ");append_n(&p,~crc);if(n<0){append(&p," error ");append_e(&p,n);}
  boot_note(line);
 }
 p=line;r=asset_open("/NOPE.TXT",9,0);append(&p,"ASSET missing ");append_e(&p,r);append(&p," ");append(&p,asset_fail);boot_note(line);
}
