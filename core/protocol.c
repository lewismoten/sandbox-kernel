/* SPDX-License-Identifier: MIT; original version-2 CDC update parser. */
#include "kernel.h"
#include "tusb.h"
#include "fatput.h"
#include "os.h"
#include "autostart.h"
#include "input.h"
volatile bool usb_reset_seen;
extern bool storage_ready,updates_ready;
extern uint32_t boot_slot,boot_generation;
static uint8_t staging[MAX_IMAGE] __attribute__((aligned(4)));
static char line[256];
static unsigned used,phase;
static uint32_t expected,received,transport_crc,activity,block_end,sector_index,install_since;
static unsigned inactive;
static uint8_t image_hash[32];
static bool executing,overflow,session,putf;
static char put_name[32];static uint8_t put_hash[32];
/* 0 command, 1 payload, 2 VERIFY drain, 3 validation, 4 write, 5 readback,
 * 6 metadata, 7 OK drain/reboot. Installation is not canceled by USB reset. */
bool protocol_busy(void) {return phase!=0;}
void protocol_reset(void) {if(phase<=2) {phase=0;used=0;overflow=false;received=0;session=false;putf=false;} }
static void fail(const char *s) {usb_reply("ERR ");usb_reply(s);usb_reply("\n");error(s);phase=0;used=0;overflow=false;putf=false;}
static void field(uint32_t n) {char b[12];number(b,n);usb_reply(b);}
static bool token(const char **p,const char *s) {size_t n=strlen(s);if(memcmp(*p,s,n)||(*p)[n]!=' ')return false;*p+=n+1;return true;}
static bool decimal(const char **p,uint32_t *v) {
 uint32_t n=0;unsigned count=0;while(**p>='0'&&**p<='9') {unsigned d=(unsigned)(*(*p)++-'0');if(++count>10||n>(~0u-d)/10)return false;n=n*10+d;}*v=n;return count>0;
}
static bool hex(const char **p,uint8_t *out,unsigned bytes) {
 for(unsigned i=0;i<bytes*2;i++) {unsigned c=(uint8_t)*(*p)++;if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;else return false;out[i/2]=(uint8_t)((out[i/2]<<4)|c);}
 return true;
}
static bool name_token(const char **p,char out[13]) {
 unsigned n=0;while(**p&&**p!=' ') {if(n==12)return false;out[n++]=*(*p)++;}out[n]=0;
 uint8_t short_name[11];return n&&!fatput_name(out,short_name);
}
static bool path_token(const char **p,char out[32]) {
 unsigned n=0;while(**p&&**p!=' ') {if(n==31)return false;out[n++]=*(*p)++;}out[n]=0;
 return n&&!fatput_check(out);
}
static bool in_os(const char *path) {while(*path)if(*path++=='/')return false;return true;}
static void reply_path(const char *path) {if(in_os(path)||!strncmp(path,"APPS/",5))usb_reply("/OS");usb_reply("/");usb_reply(path);}
/* PUTF <PATH> <size> <crc32> <sha256>: a development file for /OS/<NAME>, or
   for /GAMES/<DIR>/<NAME> when PATH is GAMES/<DIR>/<NAME>, sent
   like PUT2 in 1024-byte blocks, checked by CRC and SHA-256, then written by
   core/fatput.c. RUN <NAME> starts /OS/<NAME> as the OS program. */
static void file_command(const char *p) {
 uint32_t size;uint8_t crc[4]={0};
 if(!path_token(&p,put_name)||*p++!=' '||!decimal(&p,&size)||*p++!=' '||!hex(&p,crc,4)||*p++!=' '||!hex(&p,put_hash,32)||*p) {fail("command");return;}
 if(!size||size>FATPUT_MAX) {fail("size");return;}
 if(!session||!storage_ready||control[4]!=NO_SLOT||os_running()) {fail("not-ready");return;}
 expected=size;transport_crc=((uint32_t)crc[0]<<24)|((uint32_t)crc[1]<<16)|((uint32_t)crc[2]<<8)|crc[3];received=0;block_end=size<1024?size:1024;activity=micros();
 putf=true;phase=1;usb_reply("READY 1024\n");
}
static void run_command(const char *p) {
 char name[13],path[20]="/OS/";
 if(!name_token(&p,name)||*p) {fail("command");return;}
 if(!session||!storage_ready||os_running()) {fail("not-ready");return;}
 memcpy(path+4,name,strlen(name)+1);
 os_policy_state.faults=0; /* a fresh request starts a fresh restart budget */
 autostart_cancel(); /* a program started over USB never falls back to DIAG.ELF */
 if(os_start(path)) {usb_reply("ERR run ");usb_reply(os_fail);usb_reply("\n");return;}
 usb_reply("OK run ");usb_reply(path);usb_reply("\n");
}
/* Synthetic input (slice 3): KEY <code> <down|up|repeat>, AXIS <code> <value>,
   INPUT <add|remove>. Each KEY or AXIS is one frame (the change, then
   SYN_REPORT) for /dev/input/event0. */
static void input_command(const char *p,bool axis) {
 uint32_t code,mag=0;bool neg=false;struct input_change c;
 if(!session) {usb_reply("ERR not-ready\n");return;}
 if(!decimal(&p,&code)||*p++!=' '||code>0xffff) {usb_reply("ERR input command\n");return;}
 c.type=(uint16_t)(axis?EV_ABS:EV_KEY);c.code=(uint16_t)code;
 if(axis) {
  if(*p=='-') {neg=true;p++;}
  if(!decimal(&p,&mag)||*p||mag>32768) {usb_reply("ERR input value\n");return;}
  c.value=neg?-(int32_t)mag:(int32_t)mag;
 }
 else if(!strcmp(p,"down"))c.value=1; else if(!strcmp(p,"up"))c.value=0; else if(!strcmp(p,"repeat"))c.value=2;
 else {usb_reply("ERR input value\n");return;}
 int r=input_frame(&c,1);
 if(r) {usb_reply(r==-19?"ERR input no-device\n":"ERR input range\n");return;}
 usb_reply(axis?"OK axis ":"OK key ");field(code);usb_reply(" ");
 if(axis) {if(neg)usb_reply("-");field(mag);} else usb_reply(p);
 usb_reply("\n");
}
static void command_line(void) {
 line[used]=0;
 if(used>5&&!memcmp(line,"PUTF ",5)) {file_command(line+5);return;}
 if(used>4&&!memcmp(line,"RUN ",4)) {run_command(line+4);return;}
 if(used>4&&!memcmp(line,"KEY ",4)) {input_command(line+4,false);return;}
 if(used>5&&!memcmp(line,"AXIS ",5)) {input_command(line+5,true);return;}
 if(!strcmp(line,"INPUT add")||!strcmp(line,"INPUT remove")) {
  if(!session) {usb_reply("ERR not-ready\n");return;}
  if(line[6]=='a') input_add(); else input_remove();
  log_line(line[6]=='a'?"INPUT event0 added (synthetic)":"INPUT event0 removed");
  usb_reply(line[6]=='a'?"OK input add\n":"OK input remove\n");return;
 }
 if(!strcmp(line,"INFO")) {
  session=true;
  usb_reply("INFO 2 zero-w armv6 ABI=1.0 profile=zero-w-armv6 capacity=2097152 active=");field(boot_slot);
  usb_reply(" confirmed=");field(control[0]==CONTROL_MAGIC?control[6]:0);
  usb_reply(" pending=");field(control[0]==CONTROL_MAGIC&&control[4]!=NO_SLOT?control[7]:0);
  usb_reply(" active_generation=");field(boot_generation);
  usb_reply(updates_ready?" features=kernel,no-os":" features=locked");
  usb_reply(" install_timeout=120\n");return;
 }
 const char *p=line;uint32_t size,crc=0;
 if(used<10||!token(&p,"PUT2")||!token(&p,"kernel")||!token(&p,"zero-w-armv6")||!decimal(&p,&size)||*p++!=' ') {fail("command");return;}
 for(unsigned i=0;i<8;i++) {unsigned c=(uint8_t)*p++;if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;else {fail("crc-format");return;}crc=(crc<<4)|c;}
 if(*p||size<64||size>MAX_IMAGE) {fail("size");return;}
 if(!session||!storage_ready||!updates_ready||control[4]!=NO_SLOT) {fail("not-ready");return;}
 expected=size;transport_crc=crc;received=0;block_end=size<1024?size:1024;activity=micros();inactive=1-control[3];
 phase=1;usb_reply("READY 1024\n"); /* no panel, hashing or storage here */
}
static void work(void) {
 if(usb_reset_seen) {usb_reset_seen=false;protocol_reset();}
 if(phase==1&&expired(activity,15000000)) {tud_cdc_read_flush();fail("timeout");return;}
 if(phase>=2) {
  if(phase>=3&&phase<7&&tud_cdc_available()) {tud_cdc_read_flush();fail("trailing-input");return;}
  if(phase==2) {if(usb_reply_pending())return;phase=3;return;}
  if(phase==3) {
   if(tud_cdc_available()) {tud_cdc_read_flush();fail("trailing-input");return;}
   if(crc32(staging,expected)!=transport_crc) {fail("crc");return;}
   if(putf) {
    uint8_t h[32];sha256(staging,expected,h);
    if(memcmp(h,put_hash,32)) {fail("sha256");return;}
    int r=fatput(put_name,staging,expected);
    if(r) {fail(fatput_fail);return;}
    putf=false;phase=0;usb_reply("OK file ");reply_path(put_name);usb_reply(" ");field(expected);usb_reply("\n");
    char t[64]="File installed ";unsigned k=15;if(in_os(put_name)){memcpy(t+k,"/OS",3);k+=3;}t[k++]='/';memcpy(t+k,put_name,strlen(put_name)+1);log_line(t);return;
   }
   if(!image_valid(staging,expected)) {fail("manifest");return;}
   sha256(staging,expected,image_hash);sector_index=0;phase=4;install_since=micros();return;
  }
  if(expired(install_since,120000000)&&phase<7) {fail("install-timeout");return;}
  if(phase==4) {
   uint8_t b[512];memset(b,0,512);uint32_t n=expected-sector_index*512;if(n>512)n=512;memcpy(b,staging+sector_index*512,n);
   if(!file_write(&slots[inactive],sector_index,b)) {updates_ready=false;fail("write");return;}
   if(++sector_index==(expected+511)/512) {sector_index=0;phase=5;}return;
  }
  if(phase==5) {
   uint8_t b[512];uint32_t n=expected-sector_index*512;if(n>512)n=512;
   if(!file_read(&slots[inactive],sector_index,b)||memcmp(b,staging+sector_index*512,n)) {updates_ready=false;fail("readback");return;}
   for(unsigned i=n;i<512;i++)if(b[i]) {updates_ready=false;fail("padding");return;}
   if(++sector_index==(expected+511)/512)phase=6;
   return;
  }
  if(phase==6) {
   uint32_t c[128];memcpy(c,control,512);c[4]=inactive;c[5]=0;
   if(c[6]==0xfffffffeu) {fail("generation");return;}
   c[7]=c[6]+1;memcpy(c+24,image_hash,32);
   if(!control_commit(c)) {updates_ready=false;fail("commit-uncertain-query-info");return;}
   phase=7;activity=micros();usb_reply("OK pending rebooting\n");return;
  }
  if(phase==7&&expired(activity,500000))reboot();
  return;
 }
 unsigned budget=1024;
 while(budget--&&tud_cdc_available()) {
  uint8_t b;if(tud_cdc_read(&b,1)!=1)return;
  if(phase==1) {
   staging[received++]=b;activity=micros();
   if(received==expected) {phase=2;usb_reply("VERIFY\n");return;}
   if(received==block_end) {usb_reply("ACK ");field(received);usb_reply("\n");block_end+=1024;if(block_end>expected)block_end=expected;return;}
  } else if(b=='\n') {
   if(overflow) {fail("command-too-long");used=0;return;}
   command_line();used=0;return;
  } else if(b<32||b>126) {overflow=true;} else if(used<255)line[used++]=(char)b;else overflow=true;
 }
}
void protocol_task(void) {if(executing)return;executing=true;work();executing=false;}
/* Called once storage metadata exists; reads confirmed payload before PUT2. */
bool verify_confirmed(void) {uint32_t n;return control[0]==CONTROL_MAGIC&&slot_load(control[3],staging,&n);}
