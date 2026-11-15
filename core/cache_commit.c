/* SPDX-License-Identifier: MIT; original cooperative worker. Reuses the exact
 * synchronous FAT algorithm/order on a private privileged stack. The SD driver
 * yields at sector boundaries AND during controller/card busy polling; no IRQ,
 * user memory, service_io or nested worker runs on this stack. */
#ifdef HOST_TEST
#define _XOPEN_SOURCE 600
#endif
#include "kernel.h"
#include "cache_commit.h"
#include "fatput.h"
static uint32_t state,started;
static int result;
static bool inside;
static char game[9],name[13];
static const uint8_t *data;
static uint32_t bytes;
/* 16 KiB vs the FAT path's few nested 512-byte scratch arrays. Kept in kernel
   BSS, never mapped to the program. Host libc context setup needs more room. */
#ifdef HOST_TEST
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <ucontext.h>
static ucontext_t caller,worker;
static uint64_t stack[8192];
static void switch_to(bool run) {if(run) swapcontext(&caller,&worker);else swapcontext(&worker,&caller);}
#else
static uint64_t stack[2048];
struct context {uint32_t r[8],sp,lr;}; /* AAPCS r4-r11, SP, return PC */
static struct context caller,worker;
extern void cache_commit_switch(struct context *,const struct context *);
static void switch_to(bool run) {if(run) cache_commit_switch(&caller,&worker);else cache_commit_switch(&worker,&caller);}
#endif
bool cache_commit_pending(void) {return state==1;}
bool cache_commit_blocked(void) {return state==1&&!inside;}
uint32_t cache_commit_state(void) {return state;}
int cache_commit_error(void) {return result;}
static void run(void) {
 result=game[0]?fatsave_put(game,name,data,bytes):fatcache_put(name,data,bytes);
 state=result?3u:2u;
 switch_to(false);
 for(;;) {} /* a completed context is never resumed */
}
int cache_commit_start(const char *g,const char *n,const uint8_t *d,uint32_t size) {
 if(state==1)return -16;
 if(size>FATPUT_MAX)return -27;
 memcpy(game,g,9);memcpy(name,n,13);data=d;bytes=size;
#ifdef HOST_TEST
 if(getcontext(&worker))return -5;
 worker.uc_stack.ss_sp=stack;worker.uc_stack.ss_size=sizeof stack;worker.uc_link=0;
 makecontext(&worker,run,0);
#else
 memset(&worker,0,sizeof worker);
 worker.sp=(uint32_t)(uintptr_t)(stack+sizeof stack/sizeof stack[0]);
 worker.lr=(uint32_t)(uintptr_t)run;
#endif
 result=0;state=1;return 0;
}
void cache_commit_task(void) {
 if(state!=1||inside)return;
 started=micros();inside=true;switch_to(true);inside=false;
}
void cache_commit_checkpoint(bool boundary) {
 if(!inside)return;
 if(boundary||expired(started,CACHE_COMMIT_SLICE_US)) switch_to(false);
}
#if defined(HOST_TEST) && defined(__clang__)
#pragma clang diagnostic pop
#endif
