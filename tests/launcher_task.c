/* SPDX-License-Identifier: MIT. Host test of core/launcher_task.c: the real task, launcher and font code against
 * a fake directory, a fake supervisor state and fake input. Covers an idle USB STOP, a failed launch that needs
 * several cleanup quanta, and a 17th entry (GMOS.ELF) that must not be dropped by the 16-program cap. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "kernel.h"
#include "launcher.h"
#include "assets.h"
#include "autostart.h"
#include "input.h"
#include "os.h"
#include "fb.h"
#include "eink.h"

/* fakes */
struct fe {char name[13];int dir;uint32_t size;};
static struct fe dir[40];static unsigned ndir,cursor;
struct os_policy os_policy_state;uint32_t os_starts,os_stops,os_recoveries;bool os_exit_clean;char os_path[32];
static bool running,cleanup,stop_pend;static int start_result;static unsigned starts;
bool os_running(void) {return running;}
bool os_cleanup_pending(void) {return cleanup;}
bool os_stop_pending(void) {return stop_pend;}
int os_start(const char *p) {(void)p;starts++;if(start_result) {cleanup=true;return start_result;}running=true;return 0;}
void autostart_cancel(void) {}
void eink_reset(void) {}
void log_line(const char *s) {(void)s;}
struct fb_info fb0={960,540,32,960*4,960*540*4,0,0,0};uint8_t *fb_pixels;static unsigned homes;
void fb_home(void) {homes++;}
static uint16_t keys[8];static unsigned nk,kr;
bool input_key_pop(uint16_t *c) {if(kr==nk) return false;*c=keys[kr++];return true;}
void input_key_flush(void) {nk=kr=0;}
int asset_open(const char *p,uint32_t b,uint32_t f) {(void)p;(void)b;(void)f;cursor=0;return 1;}
int asset_readdir(int h,struct asset_dirent *o) {(void)h;if(cursor>=ndir) return 0;memset(o,0,sizeof *o);memcpy(o->name,dir[cursor].name,13);o->is_dir=(uint8_t)dir[cursor].dir;o->size=dir[cursor].size;cursor++;return 1;}
int asset_close(int h) {(void)h;return 0;}

static void add(const char *n) {memset(&dir[ndir],0,sizeof dir[ndir]);strcpy(dir[ndir].name,n);dir[ndir].size=100;ndir++;}
static void addn(char c,int i) {char n[13]={c,(char)('0'+i/10),(char)('0'+i%10),0};strcat(n,".ELF");add(n);}
static void reset(void) {ndir=0;running=cleanup=stop_pend=false;start_result=0;starts=0;os_exit_clean=false;launcher_close();nk=kr=0;
 /* os_stops and os_recoveries only ever count up, as in the kernel */}
static void tick(void) {launcher_task(true,1,"T");}
static void press(uint16_t k) {keys[nk++]=k;}
static void clean_exit(void) {running=false;os_stops++;os_exit_clean=true;strcpy(os_path,"/OS/GMOS.ELF");}

static void idle_stop(void) {
 reset();add("GMOS.ELF");clean_exit();tick();assert(launcher_active());
 os_recoveries++;tick();                        /* USB STOP with nothing running: recovery console takes over */
 assert(!launcher_active());                    /* its keys must not reach a hidden list */
 for(int i=0;i<3;i++) tick();assert(!launcher_active()&&starts==0);
 press(LAUNCHER_KEY_ENTER);tick();assert(starts==0);
 /* STOP during a pending clean-exit cleanup: the wish to reopen is cancelled too */
 reset();clean_exit();cleanup=true;tick();assert(!launcher_active());
 os_recoveries++;cleanup=false;tick();tick();assert(!launcher_active());
 puts("PASS launcher idle USB STOP");
}
static void failed_start_rollback(void) {
 reset();add("GMOS.ELF");clean_exit();tick();assert(launcher_active());
 start_result=-12;press(LAUNCHER_KEY_ENTER);tick();
 assert(launcher_active()&&strcmp(launcher_message(),"COULD NOT START THAT ONE")==0&&starts==1);
 tick();tick();assert(launcher_active());       /* cleanup quanta in progress: the screen stays */
 cleanup=false;start_result=0;tick();assert(launcher_active());
 press(LAUNCHER_KEY_ENTER);tick();assert(starts==2&&!launcher_active()); /* Enter retries */
 /* USB RUN taking over still closes it */
 reset();clean_exit();tick();assert(launcher_active());running=true;tick();assert(!launcher_active());
 puts("PASS launcher failed start keeps retry");
}
static void gmos_last_of_seventeen(void) {
 reset();for(int i=0;i<16;i++) addn('P',i);add("GMOS.ELF");
 clean_exit();tick();assert(launcher_active());
 assert(launcher_count()==16&&strcmp(launcher_name(0),"GMOS.ELF")==0&&strcmp(launcher_name(1),"P00.ELF")==0&&strcmp(launcher_name(15),"P14.ELF")==0);
 reset();for(int i=0;i<20;i++) addn('Q',19-i);
 clean_exit();tick();assert(launcher_count()==16&&strcmp(launcher_name(0),"Q00.ELF")==0&&strcmp(launcher_name(15),"Q15.ELF")==0);
 puts("PASS launcher keeps GMOS and the first 16 by name");
}
int main(void) {
 static uint8_t px[960*540*4];fb_pixels=px;
 idle_stop();failed_start_rollback();gmos_last_of_seventeen();return 0;
}
