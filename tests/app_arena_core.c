/* SPDX-License-Identifier: MIT; actual unlinked production unit tests. */
#include "app_arena_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void epochs(void) {
 struct aa_owner o={0};
 assert(aa_begin(&o)==0); assert(o.last==1 && o.active==1 && o.state==AA_LAUNCHING);
 assert(aa_begin(&o)==-16); assert(aa_run(&o)==0); assert(o.state==AA_RUNNING);
 assert(aa_run(&o)==-16); assert(aa_begin(&o)==-16);
 o=(struct aa_owner){.last=UINT64_MAX-1};
 assert(aa_begin(&o)==0 && o.active==UINT64_MAX);
 o.active=0;o.state=AA_IDLE;
 assert(aa_begin(&o)==-75 && o.last==UINT64_MAX && !o.active);
 puts("epochs: PASS");
}
static uint8_t *pool;
static uint32_t l1[4096],l2[U_L2][256],sections[U_L2];
static struct uregion regions[U_REGIONS];
static struct aa_space fresh(void) {
 memset(l1,0,sizeof l1);memset(l2,0,sizeof l2);memset(sections,0,sizeof sections);
 memset(regions,0,sizeof regions);memset(pool,0xa5,U_POOL_BYTES);
 return (struct aa_space){pool,l1,l2,regions,sections,0,0,0,0x00100000};
}
static void preflight(void) {
 struct aa_space s=fresh(),copy=s;
 assert(aa_preflight(&s)==0);assert(!memcmp(&s,&copy,sizeof s));
 /* Independently exhaust physical floor without any live mappings. */
 s.used=U_POOL_BYTES-AA_BYTES-AA_FB_BYTES-AA_FLOOR;
 assert(aa_preflight(&s)==0);
 s.used+=4096;assert(aa_preflight(&s)==-12);
 s=copy;s.region_count=U_REGIONS-1;assert(aa_preflight(&s)==-12);
 s=copy;s.region_count=U_REGIONS-2;assert(aa_preflight(&s)==0);
 s=copy;s.l2_count=U_L2-4;assert(aa_preflight(&s)==-12);
 s=copy;s.l2_count=U_L2-5;assert(aa_preflight(&s)==0);
 s=copy;l1[AA_BASE>>20]=0x1412;assert(aa_preflight(&s)==-12);
 l1[AA_BASE>>20]=0;l1[(AA_BASE>>20)+2]=0x1412;assert(aa_preflight(&s)==-12);
 l1[(AA_BASE>>20)+2]=0;l1[U_FB_BASE>>20]=0x1412;assert(aa_preflight(&s)==-12);
 s=fresh();regions[0]=(struct uregion){AA_BASE-4096,8192,U_POOL_PA,U_R|U_W};
 s.region_count=1;assert(aa_preflight(&s)==-12);
 s=fresh();s.used=UINT32_MAX;assert(aa_preflight(&s)==-22);
 s=fresh();s.l2_count=UINT32_MAX;assert(aa_preflight(&s)==-22);
 puts("preflight/floor/regions/L2/conflicts: PASS");
}
static void guard_and_revoke(void) {
 struct aa_space s=fresh();
 l1[(AA_BASE>>20)-1]=0x1412; /* privileged section covers preceding guard */
 assert(aa_preflight(&s)==-12);
 s=fresh();s.l2_count=1;sections[0]=5;l1[5]=0x1412;
 struct aa_owner o={0};struct aa_cleanup c={0};assert(!aa_begin(&o));
 assert(aa_quiesce(&o,&s,&c)==-22); /* don't clear a privileged/non-owned L1 */
 assert(l1[5]==0x1412 && o.state==AA_QUIESCING && !o.active);
 assert(aa_clean_step(&o,&s,&c)==-22);
 puts("preceding guard / non-owned table quarantine: PASS");
}
static void transaction(void) {
 struct aa_space s=fresh();struct aa_owner o={0};struct aa_checkpoint cp;
 assert(aa_prepare(&o,&s,1,1,&cp)==-16);
 assert(aa_begin(&o)==0);assert(aa_run(&o)==0);
 struct aa_owner before=o; memset(&cp,0x5a,sizeof cp);
 assert(aa_prepare(&o,&s,0,1,&cp)==-9);
 assert(aa_prepare(&o,&s,2,1,&cp)==-9);
 assert(aa_prepare(&o,&s,1,0,&cp)==-22);
 assert(aa_prepare(&o,&s,1,(uint64_t)AA_BYTES+1,&cp)==-22);
 assert(aa_prepare(&o,&s,1,UINT64_MAX,&cp)==-22);
 assert(!memcmp(&o,&before,sizeof o));
 for(unsigned i=0;i<sizeof cp;i++) assert(((unsigned char*)&cp)[i]==0x5a);
 assert(aa_prepare(&o,&s,1,1,&cp)==0);
 assert(s.used==AA_BYTES && s.l2_count==3 && s.region_count==1);
 assert(o.state==(AA_RUNNING|AA_PENDING) && !o.bytes);
 for(unsigned i=0;i<AA_BYTES;i++) assert(pool[i]==0);
 assert(pool[AA_BYTES]==0xa5);
 for(unsigned p=0;p<AA_BYTES/4096;p++)
  assert(l2[p/256][p%256]==(U_POOL_PA+p*4096+0x3f));
 assert(l2[2][24]==0); /* trailing guard, cap ends after page 23 */
 assert(!l1[(AA_BASE>>20)-1]);
 assert(aa_prepare(&o,&s,1,1,&cp)==-16);
 /* Modeled late-copy failure occurs before output stores; append rollback. */
 assert(aa_finish(&o,&s,&cp,false)==0);
 assert(s.used==0 && s.l2_count==0 && s.region_count==0);
 assert(o.state==AA_RUNNING && !o.pa && !o.bytes);
 for(unsigned i=0;i<3;i++) {assert(!l1[(AA_BASE>>20)+i]);assert(!sections[i]);}
 for(unsigned i=0;i<3*256;i++) assert(((uint32_t*)l2)[i]==0);
 assert(aa_prepare(&o,&s,1,AA_BYTES,&cp)==0);
 assert(aa_finish(&o,&s,&cp,true)==0);
 assert(o.state==(AA_RUNNING|AA_MAPPED) && o.bytes==AA_BYTES && o.pa==U_POOL_PA);
 pool[0]=123; pool[AA_BYTES-1]=45;before=o;
 /* Idempotence does not preflight capacity or clear already-owned bytes. */
 s.region_count=U_REGIONS;s.l2_count=U_L2;
 assert(aa_prepare(&o,&s,1,4095,&cp)==1);
 assert(pool[0]==123 && pool[AA_BYTES-1]==45 && !memcmp(&o,&before,sizeof o));
 assert(aa_prepare(&o,&s,2,1,&cp)==-9);
 s=fresh();o=(struct aa_owner){0};assert(!aa_begin(&o));assert(!aa_run(&o));
 assert(!aa_prepare(&o,&s,o.active,1,&cp));
 s.used+=4096;assert(aa_finish(&o,&s,&cp,false)==-16); /* cannot rewind later append */
 assert(s.used==AA_BYTES+4096 && (o.state&AA_PENDING));
 s.used-=4096;assert(!aa_finish(&o,&s,&cp,false));
 puts("transaction/zero-fill/PTE/rollback/stale/idempotence: PASS");
}
static void lifecycle(void) {
 struct aa_space s=fresh();struct aa_owner o={0};struct aa_cleanup c={0};struct aa_checkpoint cp;
 assert(aa_clean_step(&o,&s,&c)==-16);
 assert(!aa_begin(&o)); /* failed launch with partial pages, never ran */
 s.used=8192;memset(pool,0x61,s.used);
 assert(!aa_quiesce(&o,&s,&c));assert(!o.active && o.state==AA_QUIESCING && o.last==1);
 assert(aa_begin(&o)==-16 && aa_run(&o)==-16);
 assert(aa_prepare(&o,&s,1,1,&cp)==-16);
 assert(!aa_clean_step(&o,&s,&c));assert(c.cursor==4096 && s.used==8192);
 assert(pool[0]==0 && pool[4095]==0 && pool[4096]==0x61);
 c.disposition=1;memcpy(c.restart,"DIAG",5);
 assert(!aa_quiesce(&o,&s,&c));assert(c.cursor==4096); /* repeated STOP does not rewind */
 assert(!c.disposition && !c.restart[0]);
 assert(aa_clean_step(&o,&s,&c)==1 && o.state==AA_IDLE && !s.used && o.last==1);
 assert(!aa_begin(&o));assert(o.active==2);assert(!aa_run(&o));
 assert(aa_prepare(&o,&s,1,1,&cp)==-9); /* previous launch token */
 assert(!aa_prepare(&o,&s,2,1,&cp));assert(aa_quiesce(&o,&s,&c)==-16);
 assert(!aa_finish(&o,&s,&cp,true));memset(pool,0x7b,s.used);
 assert(!aa_quiesce(&o,&s,&c));
 for(unsigned j=0;j<3;j++) assert(!l1[(AA_BASE>>20)+j]);
 unsigned steps=0;int result;
 do {assert(aa_begin(&o)==-16);result=aa_clean_step(&o,&s,&c);steps++;assert(result>=0);}while(!result);
 assert(steps==AA_BYTES/4096 && !s.used && !s.region_count && !s.l2_count);
 for(unsigned i=0;i<AA_BYTES;i++) assert(!pool[i]);
 assert(pool[AA_BYTES]==0xa5 && o.last==2 && !o.active && !o.pa && !o.bytes);
 assert(!aa_quiesce(&o,&s,&c));assert(aa_clean_step(&o,&s,&c)==1); /* idle STOP */
 s=fresh();o=(struct aa_owner){0};assert(!aa_begin(&o));s.used=8192;
 assert(!aa_quiesce(&o,&s,&c));s.used+=4096;
 assert(aa_clean_step(&o,&s,&c)==-22 && o.state==AA_QUIESCING && !o.active);
 s.used-=4096;assert(aa_clean_step(&o,&s,&c)==-22); /* sticky error; no unsafe resume */
 puts("lifecycle/failed-launch/STOP/quarantine/quantum/reuse: PASS");
}
static void failure_matrix(void) {
 struct aa_space s=fresh();struct aa_owner o={0};struct aa_checkpoint cp;
 assert(!aa_begin(&o));assert(!aa_run(&o));assert(!aa_prepare(&o,&s,o.active,1,&cp));
 /* Every new PTE/unused tail entry and dedicated L1 checked before rollback.
  * A mismatch must leave PENDING and the changed mapping untouched. */
 for(unsigned j=0;j<3;j++) {
  for(unsigned p=0;p<256;p++) {
   uint32_t old=l2[j][p];l2[j][p]^=0x1000;
   assert(aa_finish(&o,&s,&cp,false)==-16);
   assert((o.state&AA_PENDING) && s.used==AA_BYTES && l2[j][p]==(old^0x1000));
   l2[j][p]=old;
  }
  uint32_t old=l1[(AA_BASE>>20)+j];l1[(AA_BASE>>20)+j]^=4;
  assert(aa_finish(&o,&s,&cp,false)==-16);l1[(AA_BASE>>20)+j]=old;
 }
 struct aa_checkpoint bad=cp;bad.used+=4096;assert(aa_finish(&o,&s,&bad,false)==-16);
 bad=cp;bad.l2_count++;assert(aa_finish(&o,&s,&bad,false)==-16);
 bad=cp;bad.region_count++;assert(aa_finish(&o,&s,&bad,false)==-16);
 bad=cp;bad.state=AA_IDLE;assert(aa_finish(&o,&s,&bad,false)==-16);
 assert(!aa_finish(&o,&s,&cp,false));
 assert(aa_finish(&o,&s,&cp,false)==-16); /* consumed checkpoint */
 struct aa_cleanup c={0};assert(!aa_quiesce(&o,&s,&c));assert(aa_clean_step(&o,&s,&c)==1);
 for(unsigned cycle=0;cycle<128;cycle++) {
  assert(!aa_begin(&o));assert(o.active==cycle+2u);assert(!aa_run(&o));
  struct aa_owner old=o;
  assert(aa_prepare(&o,&s,cycle+1u,1,&cp)==-9 && !memcmp(&old,&o,sizeof old));
  assert(!aa_prepare(&o,&s,o.active,1,&cp));assert(!aa_finish(&o,&s,&cp,true));
  pool[0]=0x66;pool[AA_BYTES-1]=0x88;assert(!aa_quiesce(&o,&s,&c));
  unsigned progress=0;int rc;
  do {assert(aa_prepare(&o,&s,o.last,1,&cp)==-16);assert(aa_begin(&o)==-16);
      rc=aa_clean_step(&o,&s,&c);assert(rc>=0);progress++;}while(!rc);
  assert(progress==536 && !pool[0] && !pool[AA_BYTES-1]);
 }
 /* Maximum used extent: 4096 independent bounded steps, no early reuse. */
 s=fresh();assert(!aa_begin(&o));s.used=U_POOL_BYTES;
 assert(!aa_quiesce(&o,&s,&c));unsigned count=0;int rc;
 do {rc=aa_clean_step(&o,&s,&c);assert(rc>=0);count++;}while(!rc);
 assert(count==4096 && o.last==130 && !s.used);
 for(unsigned i=0;i<U_POOL_BYTES;i++) assert(!pool[i]);
 puts("768 PTE + 3 L1 + checkpoint failures / 128 epoch cycles / full-pool scrub: PASS");
}
int main(void) {pool=malloc(U_POOL_BYTES);assert(pool);epochs();preflight();guard_and_revoke();transaction();lifecycle();failure_matrix();free(pool);return 0;}
