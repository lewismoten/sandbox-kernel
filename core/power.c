/* SPDX-License-Identifier: MIT */
#include "power.h"
struct power_state power0={.slow_seconds=60,.blank_seconds=600};
void power_reset(uint64_t now) {
 for(unsigned i=0;i<POWER_HOLDS;i++) {power0.tokens[i]=0;power0.flags[i]=0;}
 power0.slow_seconds=60;power0.blank_seconds=600;power0.last_input=now;
}
void power_input(uint64_t now) {power0.last_input=now;}
int power_acquire(uint64_t flags,uint64_t *token) {
 if(!flags||(flags&~3ull)) return -22;
 if(power0.next_token==UINT64_MAX) return -75;
 for(unsigned i=0;i<POWER_HOLDS;i++) if(!power0.tokens[i]) {
  power0.tokens[i]=++power0.next_token;power0.flags[i]=(uint32_t)flags;
  *token=power0.tokens[i];return 0;
 }
 return -24;
}
int power_release(uint64_t token) {
 if(token) for(unsigned i=0;i<POWER_HOLDS;i++) if(power0.tokens[i]==token) {
  power0.tokens[i]=0;power0.flags[i]=0;return 0;
 }
 return -9;
}
int power_timeouts(uint64_t slow,uint64_t blank,uint64_t now) {
 if(slow>86400||blank>86400) return -22;
 power0.slow_seconds=(uint32_t)slow;power0.blank_seconds=(uint32_t)blank;
 power_input(now);return 0;
}
uint32_t power_target(uint64_t now) {
 uint32_t held=0,out=0;
 for(unsigned i=0;i<POWER_HOLDS;i++) held|=power0.flags[i];
 uint64_t idle=now-power0.last_input;
 if(power0.slow_seconds&&idle>=(uint64_t)power0.slow_seconds*1000000u) out|=POWER_NO_SLOWDOWN;
 if(power0.blank_seconds&&idle>=(uint64_t)power0.blank_seconds*1000000u) out|=POWER_NO_BLANK;
 return out&~held;
}
uint32_t power_wait_us(uint64_t now,uint64_t deadline) {
 if(deadline&&deadline<=now) return 0;
 if(deadline&&deadline-now<POWER_POLL_US) return (uint32_t)(deadline-now);
 return POWER_POLL_US;
}
