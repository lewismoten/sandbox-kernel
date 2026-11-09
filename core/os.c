/* SPDX-License-Identifier: MIT; original OS supervisor and restart policy
 * (docs/design.md, faults and recovery; first form, without an OS A/B pair). */
#include "kernel.h"
#include "assets.h"
#include "abi.h"
#include "elf.h"
#include "clock.h"
#include "fb.h"
#include "os.h"
#include "autostart.h"
#include "sys.h"
#include "uspace.h"
#include "input.h"
#include "app_arena_core.h"
static struct aa_owner owner;
static struct aa_cleanup cleanup;
enum {RESTART=1,FALLBACK=2,USB_STOP=3,DIAG=4};
struct uframe os_frame;
const char *os_fail="none";
char os_path[32];
struct os_policy os_policy_state;
uint32_t os_starts,os_stops;
extern uint32_t boot_generation,boot_trial;
static bool running;
static bool stop_requested;
static int stop_result;
void os_request_stop(void) {stop_requested=true;}
bool os_stop_pending(void) {return stop_requested;}
int os_stop_result(void) {return stop_result;}
static uint64_t start_us,health_us; static uint32_t healths; static bool ready;
static uint8_t file[ELF_MAX_FILE] __attribute__((aligned(8)));
bool os_running(void) {return running;}
bool os_can_start(void) {return !running && !stop_requested && owner.state==AA_IDLE;}
bool os_cleanup_pending(void) {return owner.state==AA_QUIESCING && !cleanup.error;}
bool os_defer_fallback(void) {
 if(stop_requested || !os_cleanup_pending()) return false;
 cleanup.disposition=DIAG;return true;
}
static void append(char **p,const char *s) {while(*s)*(*p)++=*s++; **p=0;}
static void append_n(char **p,uint32_t n) {char t[12];number(t,n);append(p,t);}
static void append_hex(char **p,uint32_t n) {append(p,"0x");for(int i=28;i>=0;i-=4)*(*p)++="0123456789abcdef"[(n>>i)&15];**p=0;}
static uint64_t now(void) {return clock_us();}
static void sync_caches(void) {
#ifndef HOST_TEST
 uint32_t z=0; /* write-through D-cache: drain; then drop stale instructions and branch targets */
 __asm__ volatile("mcr p15,0,%0,c7,c10,4\n mcr p15,0,%0,c7,c5,0\n mcr p15,0,%0,c7,c5,6\n mcr p15,0,%0,c7,c5,4"::"r"(z):"memory");
#endif
}
bool os_policy_restart(struct os_policy *p,uint64_t t) {
 if(p->faults&&t-p->first_us<OS_FAULT_WINDOW_US) {p->faults=0;return false;}
 p->faults=1;p->first_us=t;return true;
}
static int reject(const char *why,int e) {
 if(owner.active) {
  memset(&os_frame,0,sizeof os_frame);sys_reset();
  int r=us_quiesce(&owner,&cleanup);
  if(r) cleanup.error=(uint32_t)-r;
 } else if(owner.state==AA_LAUNCHING) owner.state=AA_IDLE;
 char line[80],*p=line; os_fail=why;
 append(&p,"OS reject ");append(&p,os_path);append(&p," ");append(&p,why);log_line(line);
 return e;
}
static int arena_quarantine(int e) {
 /* A corrupt append frontier must never become an ordinary user return or a
    rewind across unknown maps. Keep the privileged console, not user entry. */
 running=false;memset(&os_frame,0,sizeof os_frame);sys_reset();
 owner.active=0;owner.pa=0;owner.bytes=0;owner.state=AA_QUIESCING;
 cleanup=(struct aa_cleanup){.extent=us_used(),.error=(uint32_t)-e};
 os_fail="APP ARENA COMMIT";return e;
}
int os_app_arena(uint32_t va,struct k_app_arena *r,uint32_t bytes) {
 /* Single foreground owner: neither transaction nor copy_out services I/O. */
 if(!running||stop_requested) return -16;
 struct aa_checkpoint cp;
 int e=us_arena_prepare(&owner,r->owner_generation,r->requested_bytes,&cp);
 if(e<0) return e;
 bool fresh=e==0;
 if(fresh&&(e=us_arena_validate(&owner,&cp))) return arena_quarantine(e);
 if(stop_requested) {
  if(fresh) {int rollback=us_arena_finish(&owner,&cp,false);if(rollback) return arena_quarantine(rollback);}
  return -16;
 }
 r->address=AA_BASE;r->actual_bytes=AA_BYTES;
 e=us_copy_out(va,r,bytes); /* validates whole range before first store */
 if(fresh) {
  int finish=us_arena_finish(&owner,&cp,!e);
  if(finish) return arena_quarantine(finish); /* no user return on unexpected corruption */
 }
 return e;
}
int os_start(const char *path) {
 if(!os_can_start()) return -16;
 size_t n=strlen(path); if(n>=sizeof(os_path)) return -36;
 memcpy(os_path,path,n+1);
 owner.state=AA_LAUNCHING; /* gate nested SD/protocol RUN before any I/O */
 int h=asset_open(path,(uint32_t)n,0); if(h<0) return reject(asset_fail,h);
 uint32_t size=0; asset_size(h,&size);
 if(size>ELF_MAX_FILE) {asset_close(h);return reject("ELF TOO BIG",-27);}
 uint32_t got=0; int r=0;
 while(!stop_requested&&got<size&&(r=asset_read(h,file+got,size-got))>0) got+=(uint32_t)r;
 asset_close(h);
 if(stop_requested) return reject("STOP DURING LAUNCH",-16);
 if(r<0||got!=size) return reject(r<0?asset_fail:"ELF SHORT READ",-5);
 struct elf_plan plan; const char *why=elf_check(file,size,&plan);
 if(why) return reject(why,-8); /* ENOEXEC */
 owner.state=AA_IDLE;
 r=aa_begin(&owner);if(r) return reject("APP EPOCH EXHAUSTED",r);
 sys_reset();
 if((r=us_reset(mmu_table()))||(r=elf_load(file,&plan))||(r=us_map(U_STACK_TOP-U_STACK_BYTES,U_STACK_BYTES,U_R|U_W))||(r=us_map(U_START_VA,4096,U_R))) return reject(us_fail,r);
 uint8_t *page; us_span(U_START_VA,4096,U_R,&page);
 struct k_startup_ext s={{sizeof s,K_ABI_MAJOR,K_ABI_MINOR,boot_generation,boot_trial,U_IMAGE_QUOTA,U_STACK_BYTES,SYS_HANDLES},owner.active};
 memcpy(page,&s,sizeof s);
 sync_caches();
 memset(&os_frame,0,sizeof os_frame);
 os_frame.r[0]=U_START_VA;os_frame.sp=U_STACK_TOP;os_frame.lr=U_EXIT_VA;os_frame.pc=plan.entry;os_frame.cpsr=0x10; /* User, ARM, IRQ on */
 fb_program_mode(); /* every program starts in 640x480x32, whatever the screen showed */
 if(stop_requested) return reject("STOP DURING LAUNCH",-16);
 if((r=aa_run(&owner))) return reject("APP STATE",r);
 start_us=health_us=now();healths=0;ready=false;running=true;os_starts++;
 char line[80],*p=line; append(&p,"OS start ");append(&p,os_path);append(&p," entry ");append_hex(&p,plan.entry);append(&p," pages ");append_n(&p,plan.bytes/4096);log_line(line);
 return 0;
}
static void stop(bool fault,const char *why,int64_t status) {
 running=false;os_stops++;
 memset(&os_frame,0,sizeof os_frame);
 int teardown=us_quiesce(&owner,&cleanup);
 if(teardown) {os_fail="USER CLEANUP";cleanup.error=(uint32_t)-teardown;}
 if(sys_presents&&fb_pixels) { /* what reached the screen, for comparison with the program's own CRC */
  uint32_t c=~0u;
  for(uint32_t y=0;y<fb0.height;y++) {const uint8_t *row=fb_pixels+(size_t)y*fb0.pitch; for(uint32_t i=0;i<fb0.width*(fb0.depth/8u);i++) {c^=row[i];for(unsigned j=0;j<8;j++)c=(c>>1)^((0u-(c&1))&0xedb88320u);}}
  char t[48],*q=t; append(&q,"OS screen crc ");append_n(&q,~c);log_line(t);
 }
 sys_reset();
 if(fault) fb_home(); /* a fault puts the kernel's own screen back; a clean exit leaves the last frame in the program's mode */
 char line[80],*p=line;
 if(!fault) {append(&p,"OS exit 0 ");append(&p,os_path);append(&p,"; recovery console");log_line(line);return;}
 if(!why) {append(&p,"OS exit ");uint32_t magnitude=(uint32_t)(uint64_t)status;if(status<0){append(&p,"-");magnitude=0u-magnitude;}append_n(&p,magnitude);append(&p," counts as a fault");log_line(line);}
 cleanup.decision_time=now();cleanup.exit_status=status;
 if(os_policy_restart(&os_policy_state,cleanup.decision_time)) {
  log_line("OS restart (one restart is allowed per 60 s)");
  cleanup.disposition=RESTART;memcpy(cleanup.restart,os_path,sizeof cleanup.restart);
 } else cleanup.disposition=FALLBACK;
}
static void fault(const char *kind,uint32_t fsr,uint32_t far) {
 char line[96],*p=line;
 append(&p,"OS fault ");append(&p,kind);append(&p," pc ");append_hex(&p,os_frame.pc);
 append(&p," fsr ");append_hex(&p,fsr);append(&p," addr ");append_hex(&p,far);log_line(line);
 stop(true,kind,0);
}
/* The heartbeat rules; true if the program was stopped for missing one. */
static bool overdue(uint64_t t) {
 if(sys_healths!=healths) {
  healths=sys_healths;health_us=t;
  if(!ready) {ready=true;char line[80],*p=line;append(&p,"OS ready ");append(&p,os_path);append(&p," after ");append_n(&p,(uint32_t)(t-start_us)/1000u);append(&p," ms");log_line(line);}
 }
 if(!ready&&t-start_us>=OS_READY_US) {fault("no readiness report in 10 s",0,os_frame.pc);return true;}
 if(ready&&t-health_us>=OS_HEARTBEAT_US) {fault("heartbeat missed 2 s",0,os_frame.pc);return true;}
 return false;
}
void os_handle(unsigned reason,uint32_t fsr,uint32_t far) {
 if(!running) return;
 uint64_t t=now();
 switch(reason) {
  case OS_SVC: {
   uint32_t insn=0;
   if(os_frame.cpsr&0x20u) {fault("thumb",0,os_frame.pc);return;}
   if(us_copy_in(&insn,(uint64_t)os_frame.pc-4u,4)) {fault("svc fetch",0,os_frame.pc-4u);return;}
   if((insn&0x0fffffffu)!=0x0f000000u) {os_frame.r[0]=(uint32_t)-38;overdue(t);return;} /* svc with a nonzero number */
   int32_t r=sys_call(os_frame.r);
   if(!running) return; /* arena commit quarantine revoked the saved frame */
   if(r==SYS_EXITED) {if(sys_exit_status) stop(true,NULL,sys_exit_status); else stop(false,NULL,0);return;}
   os_frame.r[0]=(uint32_t)r;overdue(t);return;
  }
  case OS_IRQ:
   overdue(t);return;
  case OS_PABT:
   if(os_frame.pc==U_EXIT_VA) { /* returned from the entry point */
    int32_t s=(int32_t)os_frame.r[0]; sys_exit_status=s;
    if(s) stop(true,NULL,s); else stop(false,NULL,0); return;
   }
   fault("prefetch abort",fsr,far);return;
  case OS_UNDEF: fault("undefined instruction",0,os_frame.pc);return;
  case OS_DABT: fault("data abort",fsr,far);return;
  default: fault("unknown exception",reason,0);return;
 }
}
/* A program waiting in sleep_until is not entered until its deadline; the
   heartbeat rules still run while it waits. True: leave it asleep this pass. */
bool os_asleep(void) {
 if(!running||!sys_wake_us) return false;
 uint64_t t=now();
 if(t>=sys_wake_us) {sys_wake_us=0;return false;}
 overdue(t); return true;
}
#ifndef HOST_TEST
extern unsigned user_enter(struct uframe *frame,uint32_t ttbr);
#endif
void os_task(void) {
 if(stop_requested && cleanup.disposition!=USB_STOP) {
  /* Only called by the main loop: user_enter has restored privileged
     TTBR/SP/registers and IRQs, and any syscall/SD operation has returned.
     Keep pending true during framebuffer service_io to block new commands. */
  stop_result=running?1:0;
  if(running) {running=false;os_stops++;}
  memset(&os_frame,0,sizeof os_frame);
  /* A repaired privileged-template conflict may be retried only after the
     captured extent was completely scrubbed and its counters retired. Other
     quarantine errors never rewind or release potentially dirty pages. */
  if(owner.state==AA_QUIESCING && cleanup.error && !us_used() &&
     cleanup.cursor==cleanup.extent) owner.state=AA_IDLE;
  int r=us_quiesce(&owner,&cleanup);
  cleanup.disposition=USB_STOP;
  if(r) {cleanup.error=(uint32_t)-r;os_fail="USER CLEANUP";}
  autostart_cancel();os_policy_state=(struct os_policy){0};
  sys_reset();
  bool present=input_present();input_remove();if(present) input_add();
  ready=false;healths=0;start_us=health_us=0;
  fb_home();
 }
 if(owner.state==AA_QUIESCING) {
  int r=us_clean_step(&owner,&cleanup);
  if(r==1) {
   r=us_reset(mmu_table());
   if(r) {owner.state=AA_QUIESCING;cleanup.error=(uint32_t)-r;os_fail=us_fail;}
   else r=1;
  }
  if(r<0) {
   if(stop_requested) {stop_result=r;stop_requested=false;cleanup.disposition=0;log_line("OS stop cleanup failed; program revoked");}
   return;
  }
  if(!r) return;
  uint32_t intent=cleanup.disposition;cleanup.disposition=0;
  if(stop_requested) {
   log_line(stop_result?"OS stopped by USB; recovery console":"OS already idle; recovery console");
   stop_requested=false;return;
  }
  if(intent==RESTART) {
   if(!os_start(cleanup.restart)) return;
   if(owner.state==AA_QUIESCING) {cleanup.disposition=FALLBACK;return;}
  }
  if(intent==DIAG) {
   if(!os_start(AUTOSTART_FALLBACK)) return;
   log_line("OS stopped; kernel recovery console kept");return;
  }
  if(intent==RESTART || intent==FALLBACK) {
   if(autostart_fallback()) return;
   log_line("OS stopped; kernel recovery console kept");
  }
  return;
 }
#ifndef HOST_TEST
 if(!running||os_asleep()) return;
 unsigned reason=user_enter(&os_frame,(uint32_t)(uintptr_t)us_table);
 __asm__ volatile("cpsie i":::"memory");
 uint32_t fsr=0,far=0;
 if(reason==OS_DABT) __asm__ volatile("mrc p15,0,%0,c5,c0,0\n mrc p15,0,%1,c6,c0,0":"=r"(fsr),"=r"(far));
 if(reason==OS_PABT) __asm__ volatile("mrc p15,0,%0,c5,c0,1\n mrc p15,0,%1,c6,c0,2":"=r"(fsr),"=r"(far));
 os_handle(reason,fsr,far);
#endif
}
