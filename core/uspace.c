/* SPDX-License-Identifier: MIT; original ARMv6 user address space builder.
 * Descriptor bits from the ARM1176JZF-S TRM (DDI 0301H, section 6.11, XP=1):
 * coarse L1 = base|01; small page = base|AP|C|1x with bit 0 = XN,
 * AP[1:0] at bits 5:4 and APX at bit 9. Every user page is TEX=000 C=1 B=1
 * (write-back; write-through until 2026-11-07, which made stores, and so the
 * JS runtime and drawing, about 1.6x slower). The kernel's alias of the pool
 * stays write-through; the ARM1176 D-cache is physically tagged, so both
 * views hit the same lines, and nothing here uses DMA. */
#include "kernel.h"
#include "uspace.h"
#include "app_arena_core.h"
struct uregion us_regions[U_REGIONS];
unsigned us_count;
uint32_t us_table[4096] __attribute__((aligned(16384)));
uint32_t us_l2[U_L2][256] __attribute__((aligned(1024)));
const char *us_fail="none";
static uint32_t used,l2_used,l2_section[U_L2];
#ifdef HOST_TEST
static uint8_t host_pool[U_POOL_BYTES] __attribute__((aligned(4096)));
uint8_t *us_pool(void) {return host_pool;}
uint32_t us_l2_pa(unsigned i) {return 0x0e000000u+i*1024u;} /* stand-in; tests read us_l2 directly */
#else
uint8_t *us_pool(void) {return (uint8_t *)(uintptr_t)U_POOL_PA;}
uint32_t us_l2_pa(unsigned i) {return (uint32_t)(uintptr_t)us_l2[i];}
#endif
uint32_t us_used(void) {return used;}
static struct aa_space cleanup_view(void) {
 return (struct aa_space){us_pool(),us_table,us_l2,us_regions,l2_section,
                         used,l2_used,us_count,us_l2_pa(0)};
}
int us_arena_prepare(struct aa_owner *o,uint64_t epoch,uint64_t requested,struct aa_checkpoint *cp) {
 struct aa_space s=cleanup_view();int r=aa_prepare(o,&s,epoch,requested,cp);
 used=s.used;l2_used=s.l2_count;us_count=s.region_count;return r;
}
int us_arena_finish(struct aa_owner *o,const struct aa_checkpoint *cp,bool publish) {
 struct aa_space s=cleanup_view();int r=aa_finish(o,&s,cp,publish);
 used=s.used;l2_used=s.l2_count;us_count=s.region_count;return r;
}
int us_quiesce(struct aa_owner *o,struct aa_cleanup *c) {
 struct aa_space s=cleanup_view();return aa_quiesce(o,&s,c);
}
int us_clean_step(struct aa_owner *o,struct aa_cleanup *c) {
 struct aa_space s=cleanup_view();int r=aa_clean_step(o,&s,c);
 used=s.used;l2_used=s.l2_count;us_count=s.region_count;return r;
}
static int fail(const char *s,int e) {us_fail=s;return -e;}
static bool user_section(uint32_t s) { /* MiB sections a user mapping may use */
 uint32_t a=s<<20;
 return (a>=U_IMAGE_BASE&&a<U_IMAGE_END)||(a>=U_FB_BASE&&a<U_FB_BASE+(4u<<20))||a==(U_START_VA&0xfff00000u)||(a>=U_STACK_TOP-U_STACK_BYTES-(1u<<20)&&a<U_STACK_TOP);
}
int us_reset(const uint32_t *k) {
 memset(us_regions,0,sizeof(us_regions));us_count=0;used=0;l2_used=0;
 memset(us_l2,0,sizeof(us_l2));
 for(unsigned i=0;i<4096;i++) {
  if((user_section(i)||(i>=AA_BASE>>20&&i<=(AA_BASE+AA_BYTES-1u)>>20))&&k[i]) {memset(us_table,0,sizeof(us_table));return fail("USER MAP CONFLICT",22);}
  us_table[i]=k[i];
 }
 us_fail="none";return 0;
}
static uint32_t page_bits(uint32_t prot) {
 uint32_t d=0x2u|0x8u|0x4u; /* small page, C=1 B=1 TEX=000: write-back (the D-cache is physically tagged, so the kernel's write-through alias sees the same lines) */
 if(prot&U_W) d|=0x30u; else d|=0x200u|0x20u; /* AP=11 full; else APX=1 AP=10 read-only for both */
 if((prot&U_W)||!(prot&U_X)) d|=1u; /* XN: data is never executable */
 return d;
}
int us_map(uint32_t va,uint32_t bytes,uint32_t prot) {
 if(!bytes||va&0xfffu||bytes>U_POOL_BYTES||(uint64_t)va+bytes>0x80000000ull) return fail("USER MAP RANGE",22);
 if(!prot||(prot&~(U_R|U_W|U_X))||((prot&U_W)&&(prot&U_X))) return fail("USER MAP PROT",22);
 uint32_t pages=(bytes+4095u)/4096u,end=va+pages*4096u;
 for(uint32_t s=va>>20;s<=(end-1)>>20;s++) if(!user_section(s)) return fail("USER MAP RANGE",22);
 for(unsigned i=0;i<us_count;i++) {struct uregion *r=&us_regions[i]; if(va<r->va+r->bytes&&r->va<end) return fail("USER MAP OVERLAP",22);}
 if(us_count==U_REGIONS) return fail("USER MAP REGIONS",12);
 if(pages*4096u>U_POOL_BYTES-used) return fail("USER POOL",12);
 unsigned need=0; /* sections that still need an L2 table */
 for(uint32_t s=va>>20;s<=(end-1)>>20;s++) {bool have=false;for(unsigned j=0;j<l2_used;j++) if(l2_section[j]==s) have=true; if(!have) need++;}
 if(need>U_L2-l2_used) return fail("USER L2",12);
 uint32_t pa=U_POOL_PA+used; memset(us_pool()+used,0,pages*4096u); used+=pages*4096u;
 us_regions[us_count++]=(struct uregion){va,pages*4096u,pa,prot};
 for(uint32_t p=0;p<pages;p++) {
  uint32_t v=va+p*4096u,s=v>>20; unsigned j=0;
  while(j<l2_used&&l2_section[j]!=s) j++;
  if(j==l2_used) {l2_section[j]=s;l2_used++;us_table[s]=(us_l2_pa(j)&~0x3ffu)|0x1u;} /* coarse, domain 0 */
  us_l2[j][(v>>12)&0xffu]=((pa+p*4096u)&~0xfffu)|page_bits(prot);
 }
 return 0;
}
int64_t us_span(uint32_t va,uint32_t bytes,uint32_t need,uint8_t **ptr) {
 for(unsigned i=0;i<us_count;i++) {
  struct uregion *r=&us_regions[i];
  if(va>=r->va&&va-r->va<r->bytes) {
   if((need&U_W)&&!(r->prot&U_W)) return -14;
   if((need&U_R)&&!(r->prot&(U_R|U_W|U_X))) return -14;
   uint32_t off=va-r->va,n=r->bytes-off; if(n>bytes) n=bytes;
   *ptr=us_pool()+(r->pa-U_POOL_PA)+off; return n;
  }
 }
 return -14;
}
int us_check(uint64_t va,uint64_t bytes,uint32_t need) {
 if(va>>32||bytes>>32||va+bytes>0x80000000ull) return -14; /* upper bits, wraparound, kernel half */
 uint32_t a=(uint32_t)va,left=(uint32_t)bytes;
 while(left) {uint8_t *p; int64_t n=us_span(a,left,need,&p); if(n<=0) return -14; a+=(uint32_t)n; left-=(uint32_t)n;}
 return 0;
}
int us_copy_in(void *dst,uint64_t va,uint32_t bytes) {
 if(us_check(va,bytes,U_R)) return -14;
 uint32_t a=(uint32_t)va,left=bytes; uint8_t *d=dst;
 while(left) {uint8_t *p; int64_t n=us_span(a,left,U_R,&p); memcpy(d,p,(size_t)n); d+=n; a+=(uint32_t)n; left-=(uint32_t)n;}
 return 0;
}
int us_copy_out(uint64_t va,const void *src,uint32_t bytes) {
 if(us_check(va,bytes,U_W)) return -14;
 uint32_t a=(uint32_t)va,left=bytes; const uint8_t *s=src;
 while(left) {uint8_t *p; int64_t n=us_span(a,left,U_W,&p); memcpy(p,s,(size_t)n); s+=n; a+=(uint32_t)n; left-=(uint32_t)n;}
 return 0;
}
