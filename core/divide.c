/* SPDX-License-Identifier: MIT; original ARM EABI unsigned division helpers.
 * AAPCS returns a uint64_t in r0:r1, matching quotient:remainder uidivmod. */
#include <stdint.h>
/* External EABI entry points, also called implicitly by the compiler. */
uint64_t __aeabi_uidivmod(uint32_t n,uint32_t d);
uint32_t __aeabi_uidiv(uint32_t n,uint32_t d);
uint64_t __aeabi_uidivmod(uint32_t n,uint32_t d) {
 uint32_t q=0,r=0;if(!d) return 0;
 for(int i=31;i>=0;i--) {uint32_t carry=r>>31;r=(r<<1)|((n>>i)&1);if(carry||r>=d) {r-=d;q|=1u<<i;}}
 return ((uint64_t)r<<32)|q;
}
uint32_t __aeabi_uidiv(uint32_t n,uint32_t d) {return (uint32_t)__aeabi_uidivmod(n,d);}
