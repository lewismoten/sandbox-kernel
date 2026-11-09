/* SPDX-License-Identifier: MIT; test-only real map/copy failures and queued STOP.
 * No fake allocator; all production phases run before injection. */
#define us_map probe_real_map
#define us_arena_prepare probe_real_arena_prepare
#define us_copy_out probe_real_copy_out
#include "../core/uspace.c"
#undef us_map
#undef us_arena_prepare
#undef us_copy_out
#include "os.h"
static bool late_copy_failure,stop_after_prepare,corrupt_after_prepare;
void probe_corrupt_after_prepare(void) {corrupt_after_prepare=true;}
void probe_late_copy_failure(void) {late_copy_failure=true;}
void probe_stop_after_prepare(void) {stop_after_prepare=true;}
int us_copy_out(uint64_t va,const void *src,uint32_t bytes) {
 if(late_copy_failure) {late_copy_failure=false;return -14;}
 return probe_real_copy_out(va,src,bytes);
}
int us_arena_prepare(struct aa_owner *o,uint64_t epoch,uint64_t requested,struct aa_checkpoint *cp) {
 int r=probe_real_arena_prepare(o,epoch,requested,cp);
 if(corrupt_after_prepare&&r==0) {corrupt_after_prepare=false;us_table[AA_BASE>>20]^=0x400;}
 if(stop_after_prepare) {stop_after_prepare=false;os_request_stop();}
 return r;
}
uint32_t probe_l2_count(void) {return l2_used;}
static unsigned map_n,fail_n;
void probe_fail_map(unsigned n) {map_n=0;fail_n=n;}
int us_map(uint32_t va,uint32_t bytes,uint32_t prot) {
 if(++map_n==fail_n) {us_fail="INJECTED MAP";return -12;}
 return probe_real_map(va,bytes,prot);
}
