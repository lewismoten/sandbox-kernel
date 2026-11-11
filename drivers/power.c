/* SPDX-License-Identifier: MIT; firmware tags per Raspberry Pi property interface:
 * https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
 * ARM clock id 3; GET_MIN/RATE, SET_RATE; framebuffer Blank Screen.
 * Original code; no imported implementation. */
#include "kernel.h"
#include "clock.h"
#include "input.h"
#include "fb.h"
#include "power.h"
static uint32_t fast_hz,slow_hz,activity,applied,last_target;
static uint32_t pending,pending_target,pending_mode,uncertain,mode_generation;
static uint64_t retry_at;
void power_hw_init(void) {
 uint32_t fast[2]={3,0},slow[2]={3,0};
 if(property(0x30002,fast,2)) fast_hz=fast[1];
 if(property(0x30007,slow,2)) slow_hz=slow[1];
 if(!slow_hz) slow_hz=700000000u;
 if(fast_hz&&slow_hz>fast_hz) slow_hz=fast_hz;
 mode_generation=fb_mode_generation;activity=input_activity;power_reset(clock_us());
}
static void clock_log(uint32_t hz) {
 char line[48]="POWER ARM Hz ",n[12];number(n,hz);
 memcpy(line+13,n,strlen(n)+1);log_line(line);
}
void power_hw_task(void) {
 uint64_t now=clock_us();
 if(activity!=input_activity) {activity=input_activity;power_input(now);retry_at=0;}
 uint32_t target=power_target(now);
 if(mode_generation!=fb_mode_generation) {
  mode_generation=fb_mode_generation;
  if((target|applied|pending)&POWER_NO_BLANK) uncertain|=POWER_NO_BLANK;
 }
 if(target!=last_target) {retry_at=0;last_target=target;}
 if(pending) {
  uint32_t reply[2]={0,0};
  int status=property_async_poll(reply,pending==POWER_NO_BLANK?1:2);
  if(!status) return;
  if(status==-110) {log_line("POWER firmware reply late; still waiting");return;}
  bool valid=status==1;
  if(pending==POWER_NO_BLANK) valid=valid&&reply[0]==((pending_target&pending)!=0);
  else valid=valid&&reply[0]==3&&reply[1]!=0;
  if(valid) {
   applied=(applied&~pending)|(pending_target&pending);uncertain&=~pending;
   if(pending==POWER_NO_BLANK&&pending_mode!=mode_generation) uncertain|=POWER_NO_BLANK;
   if(pending==POWER_NO_SLOWDOWN) clock_log(reply[1]);
   else {
    bool standby=(applied&POWER_NO_BLANK)!=0;
    log_line(standby?"POWER HDMI blank, CEC standby":"POWER HDMI wake, CEC active source");
    if(!standby||(target&POWER_NO_BLANK)) cec_hw_power(standby);
   }
  } else {
   uncertain|=pending;log_line("POWER firmware change failed");retry_at=now+5000000u;
  }
  pending=0;
 }
 if((target==applied&&!uncertain)||now<retry_at) return;
 uint32_t change=((target^applied)|uncertain)&POWER_NO_SLOWDOWN;
 if(change) {
  if(!fast_hz) {log_line("POWER ARM clock unavailable");retry_at=now+5000000u;return;}
  uint32_t rate[3]={3,(target&change)?slow_hz:fast_hz,0};
  if(!property_async_start(0x38002,rate,3)) return;
 } else {
  change=POWER_NO_BLANK;
  uint32_t blank[1]={(target&change)!=0};
  if(!property_async_start(0x40002,blank,1)) return;
 }
 pending=change;pending_target=target;pending_mode=mode_generation;
}
