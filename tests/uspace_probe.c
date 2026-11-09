/* SPDX-License-Identifier: MIT; only injected boundary is a real map failure. */
#define us_map probe_real_map
#include "../core/uspace.c"
#undef us_map
static unsigned map_n,fail_n;
void probe_fail_map(unsigned n) {map_n=0;fail_n=n;}
int us_map(uint32_t va,uint32_t bytes,uint32_t prot) {
 if(++map_n==fail_n) {us_fail="INJECTED MAP";return -12;}
 return probe_real_map(va,bytes,prot);
}
