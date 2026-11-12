/* SPDX-License-Identifier: MIT. The foreground side of the idle screen and launcher (include/launcher.h):
 * shows it after an OS program exits cleanly (or when none autostarted), lists /OS once on entry and
 * again only on Enter with an empty list, feeds it the keys, starts what is chosen through the
 * supervisor. Runs from the main loop only; the SD card is never touched from an interrupt. */
#include "kernel.h"
#include "launcher.h"
#include "assets.h"
#include "autostart.h"
#include "input.h"
#include "os.h"
#include "fb.h"
#include "eink.h"
static bool want;
static uint32_t seen_stops;
static const char *stopped_path;
static bool before(const char *a,const char *b) {int ra=!strcmp(a,"GMOS.ELF")?0:1,rb=!strcmp(b,"GMOS.ELF")?0:1;return ra!=rb?ra<rb:strcmp(a,b)<0;}
/* The .ELF files in /OS, the files os_start accepts: GMOS.ELF first, then by name. */
static unsigned list_os(char names[LAUNCHER_MAX][LAUNCHER_NAME]) {
 unsigned n=0;int h=asset_open("/OS/",4,0);if(h<0) return 0;
 struct asset_dirent d;
 while(n<LAUNCHER_MAX&&asset_readdir(h,&d)==1) {
  unsigned len=(unsigned)strlen(d.name);
  if(d.is_dir||len<5||len>12||memcmp(d.name+len-4,".ELF",4)||!d.size) continue;
  unsigned at=n;
  while(at&&before(d.name,names[at-1])) {memcpy(names[at],names[at-1],LAUNCHER_NAME);at--;}
  memcpy(names[at],d.name,len+1);n++;
 }
 asset_close(h);return n;
}
static int run_program(const char *path) {
 os_policy_state.faults=0; /* a fresh request starts a fresh restart budget, as USB RUN does */
 autostart_cancel();
 return os_start(path);
}
static const struct launcher_ops real={list_os,run_program};
void launcher_request(const char *stopped) {want=true;stopped_path=stopped;}
/* The string for the e-ink "USB:" line while the screen is up, else NULL. */
const char *launcher_eink_state(void) {return launcher_active()?"OS STOPPED-ENTER":NULL;}
void launcher_task(bool storage_ready,uint32_t generation,const char *build_id) {
 if(os_stops!=seen_stops) {seen_stops=os_stops;want=os_exit_clean;stopped_path=os_path;}
 bool idle=!os_running()&&!os_cleanup_pending()&&!os_stop_pending();
 if(!idle) {if(os_running()) want=false;if(launcher_active()) launcher_close();return;} /* USB RUN or STOP took over; a cleanup in progress keeps the wish */
 if(!launcher_active()) {
  if(!want||!storage_ready||!fb_pixels) return;
  want=false;
  char g[40]="KERNEL GEN ";unsigned n=11;char num[12];unsigned k=0;uint32_t v=generation;
  do {num[k++]=(char)('0'+v%10);v/=10;} while(v);
  while(k) g[n++]=num[--k];
  g[n++]=' ';for(unsigned i=0;build_id&&build_id[i]&&n<39&&i<12;i++) g[n++]=build_id[i];g[n]=0;
  launcher_footer(g);
  eink_reset();input_key_flush();fb_home();
  launcher_open(&real,stopped_path);launcher_draw(&fb0,fb_pixels);
  log_line("Kernel launcher: choose a program (Up/Down, Enter)");
  return;
 }
 uint16_t code;bool redraw=false;
 while(input_key_pop(&code)) {
  redraw|=launcher_key(code);
  if(!launcher_active()) return;
 }
 if(redraw&&fb_pixels) launcher_draw(&fb0,fb_pixels);
}
