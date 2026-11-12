/* SPDX-License-Identifier: MIT; original version-2 CDC update parser. */
#include "kernel.h"
#include "tusb.h"
#include "fatput.h"
#include "bootinfo.h"
#include "bootrec.h"
#include "os.h"
#include "autostart.h"
#include "input.h"
#ifdef USB_ROLE_HOST /* no Mac link in the host role: the CDC reads see an empty port */
static inline uint32_t tud_cdc_available(void) {return 0;}
static inline uint32_t tud_cdc_read(void *b,uint32_t n) {(void)b;(void)n;return 0;}
static inline void tud_cdc_read_flush(void) {}
#endif
#include "serial.h"
#include "shot.h"
#include "assets.h"
#include "audio_null.h"
volatile bool usb_reset_seen;
extern bool storage_ready,updates_ready;
extern uint32_t boot_slot,boot_generation;
static uint8_t staging[MAX_IMAGE] __attribute__((aligned(4)));
static char line[256];
static unsigned used,phase;
static uint32_t expected,received,transport_crc,activity,block_end,sector_index,install_since;
static unsigned inactive;
static uint8_t image_hash[32];
static bool executing,overflow,session,putf,companion;
static bool waiting_stop;
static unsigned tx_left,tx_took;static uint32_t tx_since;
#define TX_IDLE_US 2000000u /* a TX body that stalls this long is abandoned (docs/serial.md) */
static bool shot_drain,shot_limited;static uint32_t shot_since;
#define GET_PATH_MAX 192u
#define GET_MAX (4u*1024u*1024u)
static int get_handle=-1;
static uint32_t get_id,get_total,get_index,get_activity;
static bool get_directory;
static char get_path[GET_PATH_MAX+1];
static char get_request[256];static bool get_pending,get_request_directory,usblog_cmd;static unsigned usblog_idx;
static void get_close(void) {if(get_handle>=0)asset_close(get_handle);get_handle=-1;}
static char put_name[32];static uint8_t put_hash[32];
/* 0 command, 1 payload, 2 VERIFY drain, 3 validation, 4 write, 5 readback,
 * 6 metadata, 7 OK drain/reboot. Installation is not canceled by USB reset. */
bool protocol_busy(void) {return phase!=0||shot_drain||get_pending;}
void protocol_reset(void) {get_pending=false;usblog_cmd=false;get_close();shot_drain=false;shot_limited=false;if(phase<=2) {phase=0;used=0;overflow=false;received=0;session=false;putf=false;tx_left=0;} }
static void refuse(const char *s) {usb_reply("ERR ");usb_reply(s);usb_reply("\n");phase=0;used=0;overflow=false;putf=false;}
/* ERR rule: a real error (bad command, storage, CRC, fault) goes through fail(),
   which also sets the e-ink "ERR:" line (sticky). A flow-control refusal, "not-ready"
   because no INFO session or an OS is running, goes through refuse(): the reply only,
   so a normal refusal never reads as a fault on the panel. PUTF, RUN, PUT2, STOP and
   INPUT/KEY/AXIS all follow this rule. */
static void fail(const char *s) {error(s);refuse(s);}
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
static void reply_path(const char *path) {if(in_os(path)||!memcmp(path,"APPS/",5))usb_reply("/OS");usb_reply("/");usb_reply(path);}
/* PUTF <PATH> <size> <crc32> <sha256>: a development file for /OS/<NAME>, or
   for /GAMES/<DIR>/<NAME> when PATH is GAMES/<DIR>/<NAME> (or a packed game,
   GAMES/<NAME>.GMP or POCKET/<NAME>.GMP; core/fatput.c lists them), sent
   like PUT2 in 1024-byte blocks, checked by CRC and SHA-256, then written by
   core/fatput.c. RUN <NAME> starts /OS/<NAME> as the OS program. */
static void file_command(const char *p) {
 uint32_t size;uint8_t crc[4]={0};
 if(!path_token(&p,put_name)||*p++!=' '||!decimal(&p,&size)||*p++!=' '||!hex(&p,crc,4)||*p++!=' '||!hex(&p,put_hash,32)||*p) {fail("command");return;}
 if(!size||size>FATPUT_MAX) {fail("size");return;}
 if(!storage_ready) {fail("not-ready");return;}
 if(!session||control[4]!=NO_SLOT||os_running()) {refuse("not-ready");return;}
 get_close();
 expected=size;transport_crc=((uint32_t)crc[0]<<24)|((uint32_t)crc[1]<<16)|((uint32_t)crc[2]<<8)|crc[3];received=0;block_end=size<1024?size:1024;activity=micros();
 putf=true;phase=1;usb_reply("READY 1024\n");
}
/* MV <src> <dst>, RM <path>, RMDIR <path>, LS <path|/> [from]: move, delete and list on the card's FAT
   volume (core/fatput.c has the path rules and the power-cut order). A path is a real card path without
   a leading slash (GAMES/DICE/DICE.GAM, OS/APPS/HELLO); a refusal is "ERR <reason>" (a "MOV ..." reason
   is a flow-control refusal; a storage fault also sets the panel's ERR line). LS replies with ENT lines
   (at most about 360 bytes, the reply queue is 512), then "OK ls /<path> next=<n|end> skipped=<k>". */
static bool fs_path(const char **p,char out[64]) {
 unsigned n=0;while(**p&&**p!=' ') {if(n==63)return false;out[n++]=*(*p)++;}out[n]=0;return n>0;
}
static void fs_done(int r) {
 if(!r) return;
 if(!memcmp(fatput_fail,"MOV ",4)) refuse(fatput_fail); else fail(fatput_fail);
}
static void fs_command(char kind,const char *p) { /* kind: 'm' MV, 'r' RM, 'd' RMDIR, 'l' LS */
 char a[64],b[64];uint32_t from=0;
 if(!fs_path(&p,a)) {fail("command");return;}
 if(kind=='m') {if(*p++!=' '||!fs_path(&p,b)) {fail("command");return;}}
 if(kind=='l'&&*p==' ') {p++;if(!decimal(&p,&from)) {fail("command");return;}}
 if(*p) {fail("command");return;}
 if(!storage_ready) {fail("not-ready");return;}
 if(!session||control[4]!=NO_SLOT||os_running()) {refuse("not-ready");return;}
 int r;
 if(kind=='l') {
  char lines[400];uint32_t next,skipped;
  r=fatfs_ls(a,from,lines,sizeof lines-40,&next,&skipped);
  if(r<0) {fs_done(r);return;}
  usb_reply(lines);usb_reply("OK ls ");if(strcmp(a,"/"))usb_reply("/");usb_reply(a);usb_reply(" next=");
  if(next==~0u)usb_reply("end");else field(next);
  usb_reply(" skipped=");field(skipped);usb_reply("\n");return;
 }
 r=kind=='m'?fatfs_mv(a,b):kind=='r'?fatfs_rm(a):fatfs_rmdir(a);
 if(r) {fs_done(r);return;}
 usb_reply(kind=='m'?"OK mv /":kind=='r'?"OK rm /":"OK rmdir /");usb_reply(a);
 if(kind=='m') {usb_reply(" /");usb_reply(b);}
 usb_reply("\n");
 char t[160]="Card ";unsigned k=5;const char *w=kind=='m'?"move ":kind=='r'?"remove ":"rmdir ";
 memcpy(t+k,w,strlen(w));k+=(unsigned)strlen(w);
 for(const char *c=a;*c&&k<150;c++) {t[k++]=*c;}
 t[k]=0;log_line(t);
}
static void run_command(const char *p) {
 char name[13],path[20]="/OS/";
 if(!name_token(&p,name)||*p) {fail("command");return;}
 if(!storage_ready) {fail("not-ready");return;}
 if(!session||!os_can_start()) {refuse("not-ready");return;}
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
 if(!session) {refuse("not-ready");return;}
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
/* TX <len>, then len raw bytes (1..64): input for the OS's /dev/serial0. */
static void tx_command(const char *p) {
 uint32_t n;
 if(!session) {usb_reply("ERR not-ready\n");return;}
 if(!decimal(&p,&n)||*p||n<1||n>SERIAL_CHUNK) {usb_reply("ERR tx length\n");return;}
 tx_left=n;tx_took=0;tx_since=micros();
}
/* One bounded response; drain it before parsing another command or sending logs. */
static void shot_command(const char *p,bool eink) {
 uint32_t scale=1,id,offset;
 if(!session) {refuse("not-ready");return;}
 if((!eink&&(!decimal(&p,&scale)||*p++!=' '))||!decimal(&p,&id)||*p++!=' '||!decimal(&p,&offset)||*p) {refuse("shot command");return;}
 if(shot_limited&&!expired(shot_since,2000)) {refuse("shot busy");return;}
 shot_limited=true;shot_since=micros();
 struct shot_chunk s;const char *why=shot_read(eink,scale,id,offset,&s);
 if(why) {refuse(why);return;}
 usb_reply(eink?"SHOT EINK ":"SHOT FB ");
 uint32_t fields[]={s.id,s.width,s.height,s.stride,s.depth,s.order,s.total,s.offset,s.count,crc32(s.data,s.count)};
 for(unsigned i=0;i<10;i++) {field(fields[i]);usb_reply(i==9?"\n":" ");}
 usb_reply_bytes(s.data,s.count);shot_drain=true;
}
/* Card-root paths only, no normalization or parent traversal. ASCII command
 * framing deliberately leaves spaces/UTF-8 to the existing app asset API. */
static bool get_path_token(const char **p,char out[GET_PATH_MAX+1]) {
 unsigned n=0,part=0,depth=0;
 while(**p&&**p!=' ') {
  unsigned char c=(unsigned char)*(*p)++;
  if(n==GET_PATH_MAX||c<33||c>126||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;
  if(c=='/') {
   if(n&&(!part||(part==1&&out[n-1]=='.')||(part==2&&out[n-1]=='.'&&out[n-2]=='.')))return false;
   if(++depth>ASSET_MAX_DEPTH)return false;
   part=0;
  } else part++;
  out[n++]=(char)c;
 }
 out[n]=0;
 return n&&out[0]=='/'&&(n==1||(part&&!(part==1&&out[n-1]=='.')&&!(part==2&&out[n-1]=='.'&&out[n-2]=='.')));
}
static void get_command(const char *p,bool directory) {
 char path[GET_PATH_MAX+1];uint32_t id,offset;
 if(!session||!storage_ready) {refuse("not-ready");return;}
 if(!get_path_token(&p,path)||*p++!=' '||!decimal(&p,&id)||*p++!=' '||!decimal(&p,&offset)||*p) {refuse("get command");return;}
 if(shot_limited&&!expired(shot_since,2000)) {refuse("get busy");return;}
 shot_limited=true;shot_since=micros();
 if(!id) {
  if(offset) {refuse("get range");return;}
  get_close();
  get_handle=asset_open(path,(uint32_t)strlen(path),0);
  if(get_handle<0) {refuse("get open");return;}
  get_directory=directory;get_index=0;
  memcpy(get_path,path,strlen(path)+1);
  if(!++get_id)++get_id;
  if(asset_size(get_handle,&get_total)||get_total>GET_MAX||(!directory&&asset_seek(get_handle,0))) {
   get_close();refuse("get size-or-type");return;
  }
 } else if(get_handle<0||id!=get_id||directory!=get_directory||strcmp(path,get_path)) {refuse("get stale");return;}
 get_activity=micros();
 if(directory) {
  if(offset!=get_index||offset>=65536) {refuse("get range");return;}
  struct asset_dirent d;int r=asset_readdir(get_handle,&d);
  if(r<0) {get_close();refuse("get read");return;}
  usb_reply("LS ");field(get_id);usb_reply(" ");field(offset);
  if(!r) {usb_reply(" END\n");get_close();return;}
  usb_reply(d.is_dir?" D ":" F ");field(d.size);usb_reply(" ");
  /* Short aliases can contain non-ASCII on foreign cards: replace rather
   * than inject ambiguous terminal/wire text. */
  for(unsigned i=0;d.name[i];i++)if((uint8_t)d.name[i]<33||(uint8_t)d.name[i]>126) d.name[i]='?';
  usb_reply(d.name);usb_reply("\n");get_index++;return;
 }
 if(offset%SHOT_CHUNK||offset>get_total||(offset==get_total&&get_total)) {refuse("get range");return;}
 uint8_t data[SHOT_CHUNK];uint32_t count=get_total-offset;if(count>SHOT_CHUNK)count=SHOT_CHUNK;
 if(asset_seek(get_handle,offset)||asset_read(get_handle,data,count)!=(int)count) {get_close();refuse("get read");return;}
 usb_reply("GET ");uint32_t fields[]={get_id,get_total,offset,count,crc32(data,count)};
 for(unsigned i=0;i<5;i++) {field(fields[i]);usb_reply(i==4?"\n":" ");}
 usb_reply_bytes(data,count);shot_drain=true;
 if(offset+count==get_total)get_close();
}
/* Two LS forms share the name: the older GET-style "LS <path> <id> <index>" (two spaces after the
   command) and the paged "LS <path|/> [from]" (at most one). Count the spaces to tell them apart. */
static bool ls_is_old_form(void) {unsigned n=0;for(unsigned i=3;i<used;i++)if(line[i]==' ')n++;return n>=2;}
static void command_line(void) {
 line[used]=0;
 /* A GET/LS holds one of the OS's few asset handles between chunks: any other
    command ends the transfer and returns it. */
 if(!(used>4&&!memcmp(line,"GET ",4))&&!(used>3&&!memcmp(line,"LS ",3)&&ls_is_old_form())) get_close();
 if(used>4&&!memcmp(line,"GET ",4)) {memcpy(get_request,line+4,used-3);get_request_directory=false;get_pending=true;return;}
 if(used>3&&!memcmp(line,"LS ",3)&&ls_is_old_form()) {memcpy(get_request,line+3,used-2);get_request_directory=true;get_pending=true;return;}
 if(used>9&&!memcmp(line,"EINKSHOT ",9)) {shot_command(line+9,true);return;}
 if(used>7&&!memcmp(line,"FBSHOT ",7)) {shot_command(line+7,false);return;}
 if(!strcmp(line,"STOP")) {
  if(!session) {refuse("not-ready");return;}
  os_request_stop();waiting_stop=true;return;
 }
 if(!strcmp(line,"AUDIO")||(used>=5&&!memcmp(line,"TONE ",5))) {
  if(!session) {refuse("not-ready");return;}
  char r[160];if(audio_command(line,r,sizeof r)) {usb_reply(r);return;}
 }
 if(used>5&&!memcmp(line,"PUTF ",5)) {file_command(line+5);return;}
 if(used>3&&!memcmp(line,"MV ",3)) {fs_command('m',line+3);return;}
 if(used>3&&!memcmp(line,"RM ",3)) {fs_command('r',line+3);return;}
 if(used>6&&!memcmp(line,"RMDIR ",6)) {fs_command('d',line+6);return;}
 if(used>3&&!memcmp(line,"LS ",3)) {fs_command('l',line+3);return;}
 if(used>4&&!memcmp(line,"RUN ",4)) {run_command(line+4);return;}
 if(used>3&&!memcmp(line,"TX ",3)) {tx_command(line+3);return;}
 if(used>4&&!memcmp(line,"KEY ",4)) {input_command(line+4,false);return;}
 if(used>5&&!memcmp(line,"AXIS ",5)) {input_command(line+5,true);return;}
 if(!strcmp(line,"INPUT add")||!strcmp(line,"INPUT remove")) {
  if(!session) {refuse("not-ready");return;}
  if(line[6]=='a') input_add(); else input_remove();
  log_line(line[6]=='a'?"INPUT event0 added (synthetic)":"INPUT event0 removed");
  usb_reply(line[6]=='a'?"OK input add\n":"OK input remove\n");return;
 }
 if(!memcmp(line,"USBLOG ",7)&&used>7) { /* part N of /OS/SAVES/KERNEL/USB.TXT, newlines as '|' */
  unsigned i=0;for(const char *c=line+7;*c>='0'&&*c<='9'&&i<1000;c++) i=i*10+(unsigned)(*c-'0');
  usblog_idx=i;usblog_cmd=true;get_pending=true;return; /* FAT read: deferred to protocol_card_task like GET */
 }
 if(!strcmp(line,"LASTBOOT")) { /* the previous boot's record (decision, keyboards found), then this boot's */
  usb_reply("LASTBOOT name=");usb_reply(bootinfo_name());usb_reply(" previous=");usb_reply(bootinfo_prev());usb_reply(" this=");usb_reply(bootinfo_this());usb_reply("\n");return;
 }
 if(!strcmp(line,"INFO")) {
  session=true;
  usb_reply("INFO 2 zero-w armv6 ABI=1.0 profile=zero-w-armv6 capacity=2097152 active=");field(boot_slot);
  usb_reply(" confirmed=");field(control[0]==CONTROL_MAGIC?control[6]:0);
  usb_reply(" pending=");field(control[0]==CONTROL_MAGIC&&control[4]!=NO_SLOT?control[7]:0);
  usb_reply(" host_companion=");field(control[0]==CONTROL_MAGIC?control[9]:0);
  usb_reply(" active_generation=");field(boot_generation);
  usb_reply(!updates_ready?" updates=locked":control[4]!=NO_SLOT?" updates=busy":" updates=ready");
  usb_reply(os_running()?" os=running":os_starts?" os=stopped":" os=none");
  /* features= is legacy, kept so old usb_kernel.py clients still update; read updates= and os= */
  usb_reply(updates_ready?" features=kernel,no-os":" features=locked");
  usb_reply(" name=");usb_reply(bootinfo_name());usb_reply(" boot=");usb_reply(bootinfo_this());usb_reply(" lastboot=");usb_reply(bootinfo_prev());
  usb_reply(" install_timeout=120 serial=");usb_reply(serial_transport_name());usb_reply("\n");return;
 }
 const char *p=line;uint32_t size,crc=0;
 if(used<10||!token(&p,"PUT2")||(!(companion=token(&p,"host-companion"))&&!token(&p,"kernel"))||!token(&p,"zero-w-armv6")||!decimal(&p,&size)||*p++!=' ') {fail("command");return;}
 for(unsigned i=0;i<8;i++) {unsigned c=(uint8_t)*p++;if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;else {fail("crc-format");return;}crc=(crc<<4)|c;}
 if(*p||size<64||size>MAX_IMAGE) {fail("size");return;}
 if(!storage_ready) {fail("not-ready");return;}
 if(!session||!updates_ready||control[4]!=NO_SLOT) {refuse("not-ready");return;}
 get_close();
 expected=size;transport_crc=crc;received=0;block_end=size<1024?size:1024;activity=micros();inactive=1-control[3];
 phase=1;usb_reply("READY 1024\n"); /* no panel, hashing or storage here */
}
static void work(void) {
 if(get_handle>=0&&expired(get_activity,2000000))get_close();
 if(usb_reset_seen) {usb_reset_seen=false;protocol_reset();}
 /* STOP can be parsed inside SD service_io. Acknowledge only after the
    main-loop os_task boundary has revoked the old execution context. */
 if(get_pending)return;
 if(shot_drain) {if(!usb_reply_pending())shot_drain=false;return;}
 /* A capture header and body must start at a clean reply boundary. */
 if(phase==0&&usb_reply_pending())return;
 if(waiting_stop) {
  if(os_stop_pending()) return;
  waiting_stop=false;
  if(os_stop_result()<0) {usb_reply("ERR stop ");usb_reply(os_fail);usb_reply("\n");}
  else usb_reply(os_stop_result()?"OK stop\n":"OK stop idle\n");
  return;
 }
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
   if(companion&&((const uint32_t *)staging)[7]!=1) {fail("host-role-required");return;}
   /* Revoke the old companion before overwriting its slot. A power cut can only
      lose keyboard availability; the confirmed device payload is untouched. */
   if(control[9]) {
    uint32_t c[128];memcpy(c,control,512);c[9]=0;memset(c+32,0,32);
    if(!control_commit(c)) {updates_ready=false;fail("commit-uncertain-query-info");return;}
   }
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
   if(companion) {c[4]=NO_SLOT;c[9]=c[6]+1;memcpy(c+32,image_hash,32);}
   else {c[7]=c[6]+1;memcpy(c+24,image_hash,32);}
   if(!control_commit(c)) {updates_ready=false;fail("commit-uncertain-query-info");return;}
   if(companion) {phase=0;usb_reply("OK host companion installed\n");return;}
   phase=7;activity=micros();usb_reply("OK pending rebooting\n");return;
  }
  if(phase==7&&expired(activity,500000))reboot();
  return;
 }
 if(tx_left&&expired(tx_since,TX_IDLE_US)) { /* resynchronize: the host stopped mid-body; the next bytes are a command again */
  tx_left=0;tx_took=0;used=0;overflow=false;refuse("tx-timeout");
 }
 unsigned budget=1024;
 while(budget--&&tud_cdc_available()) {
  uint8_t b;if(tud_cdc_read(&b,1)!=1)return;
  if(tx_left) {
   tx_took+=serial_rx_put(&b,1);tx_since=micros();
   if(--tx_left==0) {usb_reply("OK tx ");field(tx_took);usb_reply("\n");return;}
  } else if(phase==1) {
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
/* Only the foreground main-loop boundary may enter FAT. service_io() can
 * parse a request during an OS SD operation, but must not reenter SD/assets. */
void protocol_card_task(void) {
 if(executing||!get_pending||usb_reply_pending())return;
 executing=true;get_pending=false;
 if(usblog_cmd) {
  usblog_cmd=false;char part[USBLOG_PART+1];int parts=bootrec_usblog_part(usblog_idx,part);
  if(parts<0) refuse("usblog-unreadable");
  else {usb_reply("USBLOG ");field(usblog_idx);usb_reply("/");field((uint32_t)parts);usb_reply(" ");usb_reply(part);usb_reply("\n");}
 } else get_command(get_request,get_request_directory);
 executing=false;
}
/* Called once storage metadata exists; reads confirmed payload before PUT2. */
bool verify_confirmed(void) {uint32_t n;return control[0]==CONTROL_MAGIC&&slot_load(control[3],staging,&n)&&((const uint32_t *)staging)[7]==0;}

#ifndef HOST_TEST
#if !USB_ROLE_HOST
/* The verified image uses the existing selector handoff ABI. A tiny low-RAM
   trampoline copies it over this runtime after masking IRQs and cleaning caches.
   No reset request or persistent boot selection is involved. */
extern const uint8_t companion_copy_start[],companion_copy_end[];
bool host_companion_launch(void) {
 uint32_t n;
 if(protocol_busy()||!updates_ready||control[0]!=CONTROL_MAGIC||control[4]!=NO_SLOT||!control[9])return false;
 executing=true; /* SD service_io must not parse a new update into staging */
 bool ok=slot_load(1-control[3],staging,&n)&&((const uint32_t *)staging)[7]==1;
 executing=false;
 if(!ok||!usb_host_wanted())return false; /* a link appearing during SD reads wins */
 uint32_t h[8];memcpy(h,(const void *)0x7000,sizeof h);
 h[2]=1-control[3];h[3]=BOOT_COMPANION;h[4]=control[9];h[7]=crc32(h,28);
 memcpy((void *)0x7000,h,sizeof h);
 uint32_t sdb[8];sd_export(sdb);memcpy((void *)0x7040,sdb,sizeof sdb);
 memcpy((void *)0x7800,companion_copy_start,(size_t)(companion_copy_end-companion_copy_start));
 usb_device_stop();watchdog_feed();
 uint32_t z=0;
 __asm__ volatile("mcr p15,0,%0,c7,c10,0\n mcr p15,0,%0,c7,c10,4\n mcr p15,0,%0,c7,c5,0\n mcr p15,0,%0,c7,c5,4"::"r"(z):"memory");
 ((void (*)(const void *,void *,uint32_t,uint32_t))0x7800)(staging+256,(void *)RUNTIME_BASE,n-256,RUNTIME_BASE);
 for(;;){}
}
#endif
#endif
