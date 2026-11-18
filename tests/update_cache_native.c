/* SPDX-License-Identifier: MIT
 * Production protocol + syscall + FAT COW composition, fake media/CDC only.
 * Hermes Agent / OpenAI Codex GPT-6.1 Sol (Mercury Quill). */
#include "kernel.h"
#include "assets.h"
#include "abi.h"
#include "sys.h"
#include "uspace.h"
#include "os.h"
#include "fatput.h"
#include "cache_commit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
extern void open_disk(const char *),close_disk(void),feed(const void *,unsigned);
extern const char *take_output(void);
extern uint32_t *kernel_table(void),read_count(void),write_count(void);
extern uint8_t *fb_setup(uint32_t,int);
extern void inject_failure(uint32_t);
extern bool initialize(void),updates_ready;
#define REC (U_IMAGE_BASE+0x1000)
#define PATH (U_IMAGE_BASE+0x2000)
#define DATA (U_IMAGE_BASE+0x4000)
static uint8_t staged[65536],payload[1024];
static int64_t call(unsigned n,void *record,unsigned bytes) {
 uint32_t regs[8]={REC,bytes,1,0,0,0,0,n};
 assert(!us_copy_out(REC,record,bytes));
 int64_t r=sys_call(regs);
 if(!r)assert(!us_copy_in(record,REC,bytes));
 return r;
}
static uint64_t stage(void) {
 const char path[]="/sd/OS/CACHE/GAMES.TMP";
 assert(!us_copy_out(PATH,path,sizeof path-1));
 struct k_open o={{sizeof o,0,0,0},PATH,sizeof path-1,0x601,0,0};
 assert(!call(1,&o,sizeof o));
 assert(!us_copy_out(DATA,staged,sizeof staged));
 struct k_io w={{sizeof w,0,0,0},o.fd,DATA,sizeof staged,0};
 assert(!call(3,&w,sizeof w)&&w.transferred==sizeof staged);
 return o.fd;
}
static int64_t submit(uint64_t fd) {
 const char path[]="/sd/OS/CACHE/GAMES.IDX";
 assert(!us_copy_out(PATH,path,sizeof path-1));
 struct k_cache_submit s={{sizeof s,0,0,0},fd,PATH,sizeof path-1};
 return call(22,&s,sizeof s);
}
static struct k_cache_status status(void) {
 struct k_cache_status s={{sizeof s,0,0,0},0,0};
 uint32_t rd=read_count(),wr=write_count();
 assert(!call(23,&s,sizeof s));assert(rd==read_count()&&wr==write_count());return s;
}
static void published(const void *bytes,uint32_t n) {
 const char path[]="/OS/CACHE/GAMES.IDX";
 int fd=asset_open(path,sizeof path-1,0);assert(fd>=0);
 static uint8_t got[sizeof staged+1];
 assert(asset_read(fd,got,sizeof got)==(int)n&&!memcmp(got,bytes,n));
 assert(!asset_close(fd));
}
static void command(const void *p,unsigned n,const char *wanted) {
 feed(p,n);protocol_task();assert(!strcmp(take_output(),wanted));
}
static unsigned finish(void) {
 unsigned passes=0;
 while(cache_commit_pending()) {
  protocol_task();assert(!strcmp(take_output(),""));assert(updates_ready);
  cache_commit_task();assert(++passes<20000);
 }
 return passes;
}
int main(int argc,char **argv) {
 assert(argc==5);unsigned phase=(unsigned)strtoul(argv[3],0,10);
 bool error_case=!strcmp(argv[4],"error");assert(phase>=4&&phase<=6);
 FILE *f=fopen(argv[2],"rb");assert(f);
 assert(fread(payload,1,sizeof payload,f)==sizeof payload&&fgetc(f)==EOF);fclose(f);
 uint32_t *k=kernel_table();
 for(unsigned i=0;i<128;i++)k[i]=(i<<20)|0x402|(i?8:0);
 for(unsigned i=0x200;i<0x210;i++)k[i]=(i<<20)|0x416;
 open_disk(argv[1]);assert(initialize()&&!asset_mount());fb_setup(0,1);
 assert(!os_start("/DIAG.ELF")&&os_running());
 assert(!us_map(DATA,sizeof staged,U_R|U_W));
 for(unsigned i=0;i<sizeof staged;i++)staged[i]=(uint8_t)i;
 assert(!fatcache_put("GAMES.IDX",(const uint8_t *)"old",3));
 uint64_t fd=stage();assert(!cache_commit_pending());
 feed("INFO\n",5);protocol_task();assert(strstr(take_output(),"updates=ready"));
 char line[100];int len=snprintf(line,sizeof line,"PUT2 kernel zero-w-armv6 1024 %08x\n",crc32(payload,sizeof payload));
 assert(len>0&&(unsigned)len<sizeof line);
 command(line,(unsigned)len,"READY 1024\n");command(payload,sizeof payload,"VERIFY\n");
 for(unsigned i=0;i<2+(phase>=5?2u:0u)+(phase>=6?2u:0u);i++) {
  protocol_task();assert(!strcmp(take_output(),""));
 }
 assert(os_running()&&protocol_busy()&&updates_ready&&!cache_commit_pending());
 uint32_t rd=read_count(),wr=write_count();assert(!submit(fd));
 assert(rd==read_count()&&wr==write_count()&&cache_commit_pending());
 assert(status().state==1);
 /* Both direct busy paths must overwrite an independently seeded reason. */
 assert(fatput_check("bad/name")==-22&&strcmp(fatput_fail,"busy"));
 assert(fatput("X.BIN",staged,1)==-16&&!strcmp(fatput_fail,"busy"));
 assert(fatput_check("bad/name")==-22&&strcmp(fatput_fail,"busy"));
 assert(fatcache_put("X.IDX",staged,1)==-16&&!strcmp(fatput_fail,"busy"));
 for(unsigned i=0;i<3;i++) {protocol_task();assert(!strcmp(take_output(),""));assert(updates_ready);}
 assert(rd==read_count()&&wr==write_count());
 if(error_case)inject_failure(write_count()+2);
 unsigned passes=finish();struct k_cache_status s=status();
 if(error_case) {
  assert(s.state==3&&s.error==-5);published("old",3);
  assert(submit(fd)==-9);struct k_close old={{sizeof old,0,0,0},fd};
  assert(call(12,&old,sizeof old)==-9&&call(4,&old,sizeof old)==-9);
  struct k_io w={{sizeof w,0,0,0},fd,DATA,1,0};assert(call(3,&w,sizeof w)==-9);
  assert(status().state==3);inject_failure(UINT32_MAX);
  uint64_t retry=stage();assert(retry!=fd&&!submit(retry));
  passes+=finish();s=status();
 }
 assert(s.state==2&&!s.error);published(staged,sizeof staged);
 bool done=false;
 for(unsigned i=0;i<100;i++) {
  protocol_task();const char *out=take_output();
  if(*out) {assert(!strcmp(out,"OK pending rebooting\n"));done=true;break;}
 }
 assert(done&&updates_ready&&control[4]==1);close_disk();
 printf("native real syscall/FAT phase %u %s: %u worker passes; consumed-fd/restage=%s; update READY\n",phase,error_case?"error+retry":"complete",passes,error_case?"proved":"not requested");
 return 0;
}
