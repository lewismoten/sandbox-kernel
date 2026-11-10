/* SPDX-License-Identifier: MIT; OS autostart choice and fault fallback
 * (include/autostart.h). Reads the card through the asset API only. */
#include "kernel.h"
#include "assets.h"
#include "autostart.h"
#include "os.h"
#include "fb.h"
#include "eink.h"
static char picked[20];
static bool armed; /* the running program came from autostart and is not DIAG */
static bool cancelled;
static bool present(const char *path) {
 int h=asset_open(path,(uint32_t)strlen(path),0);if(h<0)return false;asset_close(h);return true;
}
bool autostart_name(const char *t,uint32_t n,char out[20]) {
 uint32_t i=0,base=0;
 while(i<n&&(t[i]==' '||t[i]=='\t'))i++;
 memcpy(out,"/OS/",4);char *o=out+4;
 for(;i<n&&base<8;i++,base++) {
  char c=t[i];if(c>='a'&&c<='z')c=(char)(c-32);
  if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))break;
  *o++=c;
 }
 if(!base||n-i<4)return false;
 const char *ext=".ELF";
 for(unsigned k=0;k<4;k++,i++) {char c=t[i];if(c>='a'&&c<='z')c=(char)(c-32);if(c!=ext[k])return false;*o++=c;}
 *o=0;
 while(i<n&&(t[i]==' '||t[i]=='\t'||t[i]=='\r'))i++;
 if(i<n&&t[i]!='\n')return false; /* nothing else on the first line */
 return true;
}
const char *autostart_pick(void) {
 char text[64];int h=asset_open(AUTOSTART_CHOICE,sizeof AUTOSTART_CHOICE-1,0);
 if(h>=0) {
  int r=asset_read(h,text,sizeof text);asset_close(h);
  if(r>0&&autostart_name(text,(uint32_t)r,picked)&&present(picked)) {log_line("OS autostart choice from " AUTOSTART_CHOICE);return picked;}
  log_line("OS autostart choice in " AUTOSTART_CHOICE " ignored (not a present /OS/<NAME>.ELF)");
 }
 const char *d=present(AUTOSTART_DEFAULT)?AUTOSTART_DEFAULT:AUTOSTART_FALLBACK;
 memcpy(picked,d,strlen(d)+1);
 return picked;
}
int autostart_run(const char *path) {
 armed=false;os_policy_state.faults=0;
 int r=os_start(path);
 if(r&&strcmp(path,AUTOSTART_FALLBACK)) {
  error("OS autostart rejected; trying DIAG.ELF");
  if(os_defer_fallback()) return r; /* never reuse partial launch pages */
  r=os_start(AUTOSTART_FALLBACK);return r;
 }
 armed=!r&&strcmp(path,AUTOSTART_FALLBACK);
 return r;
}
void autostart_cancel(void) {armed=false;cancelled=true;}
bool autostart_cancelled(void) {return cancelled;}
bool autostart_fallback(void) {
 if(!armed)return false;
 armed=false;os_policy_state.faults=0;
 error("OS fell back to DIAG.ELF after repeated faults");
 return !os_start(AUTOSTART_FALLBACK);
}
/* Boot policy used by kernel_main: no card, no OS, so the HDMI recovery console comes up at once and says why. */
bool boot_storage_policy(bool storage_ready) {
 if(storage_ready) return true;
 eink_reset(); /* no OS will run: the panel is the kernel's (status screen) */
 log_line("SD card not readable; recovery console (USB PUTF and RUN available)");fb_recovery();return false;
}
/* The 10 s autostart: a running OS is left alone; if nothing can start, the recovery console. */
void autostart_launch(void) {
 if(!os_running()&&autostart_run(autostart_pick())) {log_line("OS none; recovery console (USB PUTF and RUN available)");fb_recovery();}
}
