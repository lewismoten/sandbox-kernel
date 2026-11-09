/* SPDX-License-Identifier: MIT; Lewis Moten, Hermes OpenAI Codex GPT-6.1 Sol.
 * Test-owned access to private counters: actual unmodified production us_map,
 * not a replacement allocator. Never built into a board image. */
#define HOST_TEST 1
#include "../core/uspace.c"
#include "app_arena_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t privileged[4096];
static struct aa_space view(void) {
 return (struct aa_space){us_pool(),us_table,us_l2,us_regions,l2_section,
                         used,l2_used,us_count,us_l2_pa(0)};
}
static void sync_view(const struct aa_space *s) {
 used=s->used;l2_used=s->l2_count;us_count=s->region_count;
}
static void reset(void) {
 memset(privileged,0,sizeof privileged);privileged[1]=0x100412;
 assert(!us_reset(privileged));memset(us_pool(),0xa5,U_POOL_BYTES);
}
static void mandatory(int framebuffer) {
 for(unsigned i=0;i<4;i++) assert(!us_map(U_IMAGE_BASE+i*(2u<<20),2u<<20,U_R|U_X));
 assert(!us_map(U_STACK_TOP-U_STACK_BYTES,U_STACK_BYTES,U_R|U_W));
 assert(!us_map(U_START_VA,4096,U_R));
 if(framebuffer) assert(!us_map(U_FB_BASE,AA_FB_BYTES,U_R|U_W));
}
static void ordered(int framebuffer) {
 reset();mandatory(framebuffer);struct aa_space s=view();
 struct aa_owner o={0};struct aa_checkpoint cp;struct aa_cleanup c={0};
 assert(s.l2_count==(framebuffer?13u:11u));assert(s.region_count==(framebuffer?7u:6u));
 unsigned prior=s.used;memset(us_pool(),0x76,prior);
 uint32_t old_l1[4096],old_l2[U_L2][256];
 memcpy(old_l1,us_table,sizeof old_l1);memcpy(old_l2,us_l2,sizeof old_l2);
 struct uregion old_regions[U_REGIONS];memcpy(old_regions,us_regions,sizeof old_regions);
 assert(!aa_begin(&o));assert(!aa_run(&o));
 /* Every modeled late failure rolls back only its append. */
 for(unsigned repeat=0;repeat<8;repeat++) {
  assert(!aa_prepare(&o,&s,o.active,1,&cp));
  memset(us_pool()+prior,0xee,AA_BYTES); /* private data before failed publication */
  assert(!aa_finish(&o,&s,&cp,false));
  assert(s.used==prior && s.l2_count==(framebuffer?13u:11u));
  assert(!memcmp(old_l1,us_table,sizeof old_l1));
  assert(!memcmp(old_l2,us_l2,sizeof old_l2));
  assert(!memcmp(old_regions,us_regions,sizeof old_regions));
  for(unsigned i=0;i<prior;i++) assert(us_pool()[i]==0x76);
  for(unsigned i=prior;i<prior+AA_BYTES;i++) assert(!us_pool()[i]);
 }
 assert(!aa_prepare(&o,&s,o.active,AA_BYTES,&cp));assert(!aa_finish(&o,&s,&cp,true));
 sync_view(&s);
 if(!framebuffer) assert(!us_map(U_FB_BASE,AA_FB_BYTES,U_R|U_W));
 s=view();assert(s.l2_count==U_L2 && s.region_count==U_REGIONS);
 assert(U_POOL_BYTES-s.used==3911680u && U_POOL_BYTES-s.used>=AA_FLOOR);
 uint8_t *p;assert(us_span(AA_BASE,1,U_W,&p)==1 && p==us_pool()+prior);
 assert(us_check(AA_BASE-4096,1,U_R)==-14);
 assert(us_check(AA_BASE+AA_BYTES,1,U_R)==-14);
 assert(aa_prepare(&o,&s,o.active,4095,&cp)==1); /* full metadata, same lease */
 assert(!aa_quiesce(&o,&s,&c));assert(us_table[1]==privileged[1]);
 unsigned steps=0;int rc;
 do {rc=aa_clean_step(&o,&s,&c);assert(rc>=0);steps++;}while(!rc);
 assert(steps==c.extent/4096 && o.last==1 && !s.used);
 for(unsigned i=0;i<c.extent;i++) assert(!us_pool()[i]);
 for(unsigned i=c.extent;i<U_POOL_BYTES;i++) assert(us_pool()[i]==0xa5);
 assert(us_table[1]==privileged[1]);
 printf("actual-uspace %s: rollback8, floor, all16 L2/all8 regions, guards, scrub PASS\n",
        framebuffer?"framebuffer-first":"arena-first");
}
static void exhausted(void) {
 reset(); /* Legal 4-segment/8MiB ELF layout, sparse and non-section-aligned. */
 for(unsigned i=0;i<4;i++) assert(!us_map(U_IMAGE_BASE+4096+i*(4u<<20),2u<<20,U_R|U_X));
 assert(!us_map(U_STACK_TOP-U_STACK_BYTES,U_STACK_BYTES,U_R|U_W));
 assert(!us_map(U_START_VA,4096,U_R));
 struct aa_space s=view(),before=s;assert(s.l2_count==15);
 uint8_t *snapshot=malloc(U_POOL_BYTES);assert(snapshot);memcpy(snapshot,us_pool(),U_POOL_BYTES);
 struct aa_owner o={0};struct aa_checkpoint cp;
 memset(&cp,0x5a,sizeof cp);assert(!aa_begin(&o));assert(!aa_run(&o));struct aa_owner owner=o;
 for(unsigned i=0;i<64;i++) assert(aa_prepare(&o,&s,o.active,1,&cp)==-12);
 assert(!memcmp(&s,&before,sizeof s) && !memcmp(&o,&owner,sizeof o));
 assert(!memcmp(snapshot,us_pool(),U_POOL_BYTES));free(snapshot);
 for(unsigned i=0;i<sizeof cp;i++) assert(((uint8_t*)&cp)[i]==0x5a);
 /* Internal us_map pressure, NOT ELF admission (exceeds image quota). */
 reset();unsigned bytes=U_POOL_BYTES-AA_BYTES-AA_FB_BYTES-AA_FLOOR;
 assert(!us_map(U_IMAGE_BASE,bytes,U_R|U_W));s=view();assert(!aa_preflight(&s));
 assert(!us_map(U_IMAGE_BASE+bytes,4096,U_R|U_W));s=view();assert(aa_preflight(&s)==-12);
 /* Region pressure independent of bytes/tables; >4 ELF segments is internal only. */
 reset();for(unsigned i=0;i<7;i++) assert(!us_map(U_IMAGE_BASE+i*8192,4096,U_R|U_W));
 s=view();assert(s.l2_count==1 && aa_preflight(&s)==-12);
 puts("actual-uspace sparse-ELF L2 exhaustion64 / internal pool-floor / regions: PASS");
}
int main(void) {ordered(0);ordered(1);exhausted();return 0;}
