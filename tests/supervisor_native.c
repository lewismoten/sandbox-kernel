/* SPDX-License-Identifier: MIT; original real-supervisor sanitizer witness.
 * Hermes Agent / OpenAI Codex GPT-6.1 Sol (openai-codex), Lewis Moten. */
#include "kernel.h"
#include "os.h"
#include "uspace.h"
#include "abi.h"
#include "sys.h"
#include "assets.h"
#include <assert.h>
#include <stdio.h>
extern void open_disk(const char *),close_disk(void),advance(uint32_t);
extern uint32_t *kernel_table(void);
extern uint8_t *fb_setup(uint32_t,int);
extern uint64_t probe_epoch(void),probe_last(void);
extern uint32_t probe_phase(void),probe_cursor(void);
extern void probe_fail_map(unsigned),probe_last_set(uint64_t);
static void drain(void) {
 unsigned n=0;while(os_cleanup_pending()) {assert(++n<=8194);os_task();}
 assert(probe_phase()==0);
}
static void stopped(void) {
 os_request_stop();os_task();drain();assert(!os_stop_pending());
 assert(!os_running()&&!probe_epoch()&&!us_used()&&!us_count);
}
static void start(const char *path) {
 assert(os_start(path)==0);assert(os_running()&&probe_epoch());
 struct k_startup s;assert(!us_copy_in(&s,U_START_VA,sizeof s));
 assert(sizeof s==40&&s.size==48&&s.major==1&&s.minor==8);
 assert(s.image_bytes==U_IMAGE_QUOTA&&s.stack_bytes==U_STACK_BYTES);
 assert(us_check(0x30000000,1,U_R)==-14);
 uint32_t r[8]={0};r[2]=1;r[7]=22;assert(sys_call(r)==-38);
}
int main(int argc,char **argv) {
 assert(argc==3);uint32_t *k=kernel_table();
 for(unsigned i=0;i<128;i++)k[i]=(i<<20)|0x402|(i?8:0);
 for(unsigned i=0x200;i<0x210;i++)k[i]=(i<<20)|0x416;
 open_disk(argv[1]);assert(!asset_mount());fb_setup(0,1);
 const char *path=argv[2];start(path);uint64_t epoch=probe_epoch();
 /* Exercise the actual loaded app's entry-return path; no user instructions run. */
 uint32_t used=us_used();os_frame.pc=U_EXIT_VA;os_frame.r[0]=0;os_handle(OS_PABT,0,0);
 assert(!os_running()&&!probe_epoch()&&probe_last()==epoch);
 assert(us_used()==used&&probe_phase()==3);assert(os_start(path)==-16);
 unsigned steps=0;while(os_cleanup_pending()){os_task();steps++;}
 assert(steps==used/4096&&!us_used());start(path);assert(probe_epoch()==epoch+1);stopped();
 if(!strcmp(path,"/OS/DIAG.ELF")) {
  /* Each map injection is at the actual loader/stack/startup call boundary. */
  for(unsigned n=1;n<=4;n++) {
   uint64_t last=probe_last();probe_fail_map(n);assert(os_start(path)==-12);
   assert(!os_running()&&!probe_epoch()&&probe_last()==last+1);
   assert(os_start(path)==-16);drain();assert(!us_used());
  }
  probe_fail_map(0);
  start(path);
  struct k_exit min_exit={{sizeof min_exit,0,0,0},INT64_MIN};
  assert(!us_copy_out(U_IMAGE_BASE+4096,&min_exit,sizeof min_exit));
  os_frame.r[0]=U_IMAGE_BASE+4096;os_frame.r[1]=sizeof min_exit;os_frame.r[2]=1;
  os_frame.r[7]=16;os_frame.pc=U_IMAGE_BASE+4;os_handle(OS_SVC,0,0);stopped();
  for(unsigned event=0;event<14;event++) {
   start(path);epoch=probe_epoch();used=us_used();
   if(event<2) {os_frame.pc=U_EXIT_VA;os_frame.r[0]=event;os_handle(OS_PABT,0,0);}
   else if(event<5) {
    struct k_exit x={{sizeof x,0,0,0},event==2?0:event==3?7:-9};
    assert(!us_copy_out(U_IMAGE_BASE+4096,&x,sizeof x));
    os_frame.r[0]=U_IMAGE_BASE+4096;os_frame.r[1]=sizeof x;os_frame.r[2]=1;os_frame.r[7]=16;
    os_frame.pc=U_IMAGE_BASE+4;os_handle(OS_SVC,0,0);
   } else if(event==5) {os_frame.cpsr=0x30;os_handle(OS_SVC,0,0);}
   else if(event==6) {os_frame.pc=4;os_handle(OS_SVC,0,0);}
   else if(event==7) {advance(OS_READY_US);os_handle(OS_IRQ,0,0);}
   else if(event==8||event==9) {
    sys_healths=1;os_handle(OS_IRQ,0,0);advance(OS_HEARTBEAT_US);
    if(event==9) {sys_wake_us=UINT64_MAX;assert(os_asleep());}
    else os_handle(OS_IRQ,0,0);
   } else os_handle(event==10?OS_UNDEF:event==11?OS_DABT:event==12?OS_PABT:99,7,U_IMAGE_BASE);
   assert(!os_running()&&!probe_epoch()&&us_used()==used&&probe_last()==epoch);
   struct uframe zero={0};assert(!memcmp(&zero,&os_frame,sizeof zero));stopped();
  }
  start(path);assert(!us_reset(k));assert(!us_map(U_IMAGE_BASE,U_POOL_BYTES,U_R|U_W));
  memset(us_pool(),0xa9,U_POOL_BYTES);os_request_stop();
  for(unsigned i=0;i<U_POOL_BYTES/4096;i++) {
   os_request_stop();os_task();assert(probe_cursor()==(i+1)*4096);
   if(i+1<U_POOL_BYTES/4096) {assert(us_used()==U_POOL_BYTES);assert(os_start(path)==-16);}
  }
  assert(!us_used());for(unsigned i=0;i<U_POOL_BYTES;i++)assert(!us_pool()[i]);
  probe_last_set(UINT64_MAX-1);start(path);assert(probe_epoch()==UINT64_MAX);stopped();
  assert(os_start(path)==-75&&!probe_epoch()&&!us_used());
 }
 close_disk();puts("actual supervisor/ELF/startup/revoke/4096-byte scrub/old-app contract PASS");return 0;
}
