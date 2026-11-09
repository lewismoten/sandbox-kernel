/* SPDX-License-Identifier: MIT; synthetic NON-AUDIO client and real-path tests.
 * Lewis Moten; Hermes Agent / OpenAI Codex GPT-6.1 Sol (openai-codex). */
#define main supervisor_prior_main
#include "supervisor_native.c"
#undef main
#include <sandbox/syscall.h>
#include <stdlib.h>
#define REC (U_STACK_TOP-4096u)
#define BASE SBX_APP_ARENA_ADDRESS
#define CAP SBX_APP_ARENA_BYTES
extern void probe_late_copy_failure(void),probe_stop_after_prepare(void);
extern unsigned probe_l2_count(void);
static unsigned transport_calls;
static bool skew;
int32_t sbx_syscall(uint32_t call,void *record,uint32_t bytes) {
 transport_calls++;
 assert(call==21&&bytes==48);
 if(skew) return -38;
 assert(!us_copy_out(REC,record,bytes));
 uint32_t regs[8]={REC,bytes,1,0,0,0,0,call};
 int e=sys_call(regs);
 if(!e) assert(!us_copy_in(record,REC,bytes));
 return e;
}
static void *short_alloc(size_t n) {return malloc(n);}
static void sdk_legacy(void) {
 /* An actual forty-byte allocation: ASan detects any ungated extension read. */
 sbx_startup *s=short_alloc(40);assert(s);
 *s=(sbx_startup){40,1,5,9,0,8u<<20,1u<<20,16};
 void *a=(void *)(uintptr_t)17;uint64_t n=19;
 assert(sbx_app_arena(s,1,&a,&n)==-38&&transport_calls==0);
 s->minor=6;assert(sbx_app_arena(s,1,&a,&n)==-38&&transport_calls==0);
 s->size=48;s->major=2;assert(sbx_app_arena(s,1,&a,&n)==-38&&transport_calls==0);
 assert(a==(void *)(uintptr_t)17&&n==19);free(s);
 sbx_startup_ext modern={{48,1,6,9,0,8u<<20,1u<<20,16},1};
 skew=true;assert(sbx_app_arena(&modern.prefix,1,&a,&n)==-38);
 assert(transport_calls==1&&a==(void *)(uintptr_t)17&&n==19);skew=false;
}
static int dispatch(struct k_app_arena *r) {
 assert(!us_copy_out(REC,r,sizeof *r));
 uint32_t regs[8]={REC,sizeof *r,1,0,0,0,0,21};
 int e=sys_call(regs);if(!e)assert(!us_copy_in(r,REC,sizeof *r));return e;
}
static struct k_app_arena request(uint64_t epoch,uint64_t n) {
 return (struct k_app_arena){{48,0,0,0},epoch,n,0,0};
}
static void envelope(uint64_t epoch) {
 uint8_t in[4096],out[4096];uint32_t l1[4096],l2[16][256];
 struct k_app_arena r=request(epoch,1);
 uint32_t used=us_used(),tables=probe_l2_count(),regions=us_count;
 memcpy(l1,us_table,sizeof l1);memcpy(l2,us_l2,sizeof l2);
 /* Every malformed record fails before ownership/allocation or any store. */
 for(unsigned kind=0;kind<17;kind++) {
  memset(in,0,sizeof in);memcpy(in,&r,48);
  uint32_t bytes=48,major=1,va=REC,extra=0;int want=-22;
  if(kind<4) in[kind*4]=kind?1:40;
  else if(kind==4) in[32]=1;
  else if(kind==5) in[40]=1;
  else if(kind==6) {bytes=56;in[0]=56;in[55]=1;}
  else if(kind==7) {major=2;want=-93;}
  else if(kind==8) extra=1;
  else if(kind==9) {va=REC+1;want=-14;}
  else if(kind==10) {va=0xfffffff8u;want=-14;}
  else if(kind==11) {va=U_STACK_TOP-40;want=-14;}
  else if(kind==12) bytes=47;
  else if(kind==13) bytes=4104;
  else if(kind==14) bytes=0;
  else if(kind==15) {va=0;want=-14;}
  else {va=U_START_VA;want=-14;}
  assert(!us_copy_out(REC,in,4096));
  uint8_t saved[48],*ro=NULL;
  if(kind==16) {assert(us_span(U_START_VA,48,U_R,&ro)==48);memcpy(saved,ro,48);memcpy(ro,in,48);memset(us_pool()+used,0x91,CAP);}
  uint32_t regs[8]={va,bytes,major,extra,0,0,0,21};
  assert(sys_call(regs)==want);
  if(ro) {memcpy(ro,saved,48);for(unsigned i=0;i<CAP;i++)assert(us_pool()[used+i]==0x91);}
  assert(!us_copy_in(out,REC,4096)&&!memcmp(in,out,4096));
  assert(us_used()==used&&probe_l2_count()==tables&&us_count==regions);
  assert(!memcmp(l1,us_table,sizeof l1)&&!memcmp(l2,us_l2,sizeof l2));
 }
 puts("dispatcher envelope/header/tail/output/range/order atomicity PASS");
}
int main(int argc,char **argv) {
 assert(argc==3);sdk_legacy();uint32_t *k=kernel_table();
 for(unsigned i=0;i<128;i++)k[i]=(i<<20)|0x402|(i?8:0);
 for(unsigned i=0x200;i<0x210;i++)k[i]=(i<<20)|0x416;
 open_disk(argv[1]);assert(!asset_mount());fb_setup(0,1);
 const char *path=argv[2];start(path);envelope(probe_epoch());
 sbx_startup_ext startup;assert(!us_copy_in(&startup,U_START_VA,48));
 assert(startup.prefix.size==48&&startup.app_generation==probe_epoch());
 uint32_t used=us_used(),tables=probe_l2_count(),regions=us_count;
 struct k_app_arena r=request(probe_epoch(),1),original=r;
 assert(!us_copy_out(REC,&r,48));
 probe_late_copy_failure();uint32_t late[8]={REC,48,1,0,0,0,0,21};assert(sys_call(late)==-14);
 assert(us_used()==used&&probe_l2_count()==tables&&us_count==regions);
 struct k_app_arena unchanged;assert(!us_copy_in(&unchanged,REC,48));assert(!memcmp(&original,&unchanged,48));
 for(unsigned i=0;i<CAP;i++)assert(!us_pool()[used+i]);
 memset(us_pool()+used,0xc7,CAP); /* first successful exposure must erase dirty free backing */
 void *address=NULL;uint64_t actual=0;
 assert(!sbx_app_arena(&startup.prefix,1,&address,&actual));
 assert((uintptr_t)address==BASE&&actual==CAP);
 assert(us_used()==used+CAP&&probe_l2_count()==tables+3&&us_count==regions+1);
 uint8_t *p;assert(us_span(BASE,CAP,U_W,&p)==CAP);
 for(unsigned i=0;i<CAP;i++)assert(!p[i]);
 memset(p,0xa7,CAP);
 uint64_t sizes[]={1,4095,4096,CAP-1,CAP};
 for(unsigned j=0;j<sizeof sizes/sizeof sizes[0];j++) {
  r=request(probe_epoch(),sizes[j]);assert(!dispatch(&r));assert(r.address==BASE&&r.actual_bytes==CAP);
  assert(us_used()==used+CAP&&probe_l2_count()==tables+3);
  for(unsigned i=0;i<CAP;i++)assert(p[i]==0xa7);
 }
 uint64_t bad[]={0,CAP+1,UINT32_MAX,UINT64_MAX};
 for(unsigned j=0;j<4;j++) {r=request(probe_epoch(),bad[j]);assert(dispatch(&r)==-22);}
 r=request(0,0);assert(dispatch(&r)==-9);
 uint64_t epoch=probe_epoch();stopped();start(path);
 r=request(epoch,1);assert(dispatch(&r)==-9);
 r=request(probe_epoch(),1);probe_stop_after_prepare();
 assert(!us_copy_out(REC,&r,48));used=us_used();
 uint32_t regs[8]={REC,48,1,0,0,0,0,21};assert(sys_call(regs)==-16);
 assert(us_used()==used);stopped();start(path);
 r=request(probe_epoch(),CAP);assert(!dispatch(&r));assert(us_span(BASE,CAP,U_R,&p)==CAP);
 for(unsigned i=0;i<CAP;i++)assert(!p[i]);
 stopped();close_disk();puts("NON-AUDIO SDK/short startup/skew/real dispatcher/rollback/STOP/epoch/PTE charge PASS");return 0;
}
