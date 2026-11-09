/* SPDX-License-Identifier: MIT; original unlinked slice A.
 * Lewis Moten; Hermes Agent / OpenAI Codex GPT-6.1 Sol (openai-codex).
 * See app_arena_core.h for exclusive ownership and serialization prerequisites.
 */
#include "app_arena_core.h"
#include <string.h>
int aa_begin(struct aa_owner *o) {
 if(o->state!=AA_IDLE || o->active || o->pa || o->bytes || o->reserved) return -16;
 if(o->last==UINT64_MAX) return -75;
 o->active=++o->last; o->state=AA_LAUNCHING; return 0;
}
int aa_run(struct aa_owner *o) {
 if(o->state!=AA_LAUNCHING || !o->active) return -16;
 o->state=AA_RUNNING; return 0;
}
static int bounds(const struct aa_space *s) {
 if(!s->pool || !s->l1 || !s->l2 || !s->regions || !s->sections ||
    s->used>U_POOL_BYTES || (s->used&4095u) || s->region_count>U_REGIONS ||
    s->l2_count>U_L2 || (s->l2_pa&1023u) ||
    (uint64_t)s->l2_pa+U_L2*1024u>0x100000000ull) return -22;
 for(unsigned j=0;j<s->l2_count;j++) if(s->sections[j]>=4096) return -22;
 return 0;
}
int aa_quiesce(struct aa_owner *o,const struct aa_space *s,struct aa_cleanup *c) {
 /* Caller has already restored the privileged table and blocked user entry.
  * Pending transaction must finish/rollback before teardown may capture extent.
  * This core clears only lease/epoch; no live table switch or handle close. */
 if(o->state&AA_PENDING) return -16;
 if(o->state!=AA_IDLE && o->state!=AA_LAUNCHING && o->state!=AA_RUNNING &&
    o->state!=(AA_RUNNING|AA_MAPPED) && o->state!=AA_QUIESCING) return -16;
 if(o->state==AA_QUIESCING) {
  c->disposition=0;memset(c->restart,0,sizeof c->restart);return 0;
 }
 o->active=0;o->pa=0;o->bytes=0;o->state=AA_QUIESCING;
 c->cursor=0;c->extent=s->used;c->error=0;
 c->disposition=0;memset(c->restart,0,sizeof c->restart);
 if(bounds(s)) {c->error=22;return -22;}
 for(unsigned j=0;j<s->l2_count;j++) {
  if(s->l1[s->sections[j]]!=((s->l2_pa+j*1024u)|1u)) {
   c->error=22;return -22;
  }
 }
 for(unsigned j=0;j<s->l2_count;j++) s->l1[s->sections[j]]=0;
 return 0;
}
int aa_clean_step(struct aa_owner *o,struct aa_space *s,struct aa_cleanup *c) {
 if(o->state!=AA_QUIESCING || o->active) return -16;
 if(c->error) return -(int)c->error;
 if(bounds(s) || c->extent!=s->used || c->cursor>c->extent ||
    (c->cursor&4095u)) {c->error=22;return -22;}
 if(c->cursor<c->extent) {
  memset(s->pool+c->cursor,0,4096);c->cursor+=4096;
 }
 if(c->cursor<c->extent) return 0;
 memset(s->l2,0,U_L2*sizeof s->l2[0]);
 memset(s->sections,0,U_L2*sizeof s->sections[0]);
 memset(s->regions,0,U_REGIONS*sizeof s->regions[0]);
 s->used=0;s->l2_count=0;s->region_count=0;
 o->state=AA_IDLE;return 1;
}
int aa_preflight(const struct aa_space *s) {
 if(bounds(s)) return -22;
 unsigned fb=0;
 for(unsigned i=0;i<s->region_count;i++) {
  const struct uregion *r=&s->regions[i];
  uint64_t end=(uint64_t)r->va+r->bytes;
  if(r->va<(uint64_t)AA_BASE+AA_BYTES+4096u && end>AA_BASE-4096u) return -12;
  if(r->va<(uint64_t)U_FB_BASE+AA_FB_BYTES && end>U_FB_BASE) {
   if(r->va!=U_FB_BASE || r->bytes!=AA_FB_BYTES || r->prot!=(U_R|U_W)) return -12;
   fb=1;
  }
 }
 /* All three dedicated arena sections must be unused, not just the pages. */
 if(s->l1[(AA_BASE>>20)-1]) return -12; /* preceding guard must be unmapped */
 for(unsigned j=0;j<3;j++) if(s->l1[(AA_BASE>>20)+j]) return -12;
 for(unsigned j=0;j<s->l2_count;j++)
  if(s->sections[j]>=AA_BASE>>20 && s->sections[j]<=(AA_BASE>>20)+2) return -12;
 if(!fb) {
  /* Reserve two unused framebuffer sections: partial maps are not supported. */
  for(unsigned j=0;j<2;j++) if(s->l1[(U_FB_BASE>>20)+j]) return -12;
 }
 uint32_t reserve=fb?0:AA_FB_BYTES;
 if((uint64_t)s->used+AA_BYTES+reserve+AA_FLOOR>U_POOL_BYTES ||
    s->region_count+1u+(fb?0u:1u)>U_REGIONS ||
    s->l2_count+3u+(fb?0u:2u)>U_L2) return -12;
 return 0;
}
int aa_prepare(struct aa_owner *o,struct aa_space *s,uint64_t epoch,
               uint64_t requested,struct aa_checkpoint *cp) {
 if((o->state!=AA_RUNNING && o->state!=(AA_RUNNING|AA_MAPPED)) || !o->active) return -16;
 if(!epoch || epoch!=o->active) return -9;
 if(!requested || requested>AA_BYTES) return -22;
 if(o->state&AA_MAPPED) return 1;
 int e=aa_preflight(s);if(e) return e;
 *cp=(struct aa_checkpoint){s->used,s->l2_count,s->region_count,o->state};
 /* Nothing below can fail or call out. No PTE precedes complete zero-fill. */
 memset(s->pool+s->used,0,AA_BYTES);
 memset(s->l2+s->l2_count,0,3*sizeof s->l2[0]);
 for(unsigned j=0;j<3;j++) {
  unsigned n=s->l2_count+j,sec=(AA_BASE>>20)+j;
  s->sections[n]=sec;s->l1[sec]=(s->l2_pa+n*1024u)|1u;
 }
 for(unsigned p=0;p<AA_BYTES/4096u;p++)
  s->l2[s->l2_count+p/256u][p%256u]=(U_POOL_PA+s->used+p*4096u)|0x3fu;
 s->regions[s->region_count++]=(struct uregion){AA_BASE,AA_BYTES,U_POOL_PA+s->used,U_R|U_W};
 s->used+=AA_BYTES;s->l2_count+=3;o->state|=AA_PENDING;return 0;
}
int aa_validate_finish(const struct aa_owner *o,const struct aa_space *s,const struct aa_checkpoint *cp) {
 if(bounds(s) || o->state!=(AA_RUNNING|AA_PENDING) || !o->active ||
    cp->state!=AA_RUNNING || cp->used>U_POOL_BYTES-AA_BYTES || (cp->used&4095u) ||
    cp->l2_count>U_L2-3 || cp->region_count>=U_REGIONS ||
    s->used!=cp->used+AA_BYTES || s->l2_count!=cp->l2_count+3 ||
    s->region_count!=cp->region_count+1) return -16;
 const struct uregion expected={AA_BASE,AA_BYTES,U_POOL_PA+cp->used,U_R|U_W};
 if(memcmp(&s->regions[cp->region_count],&expected,sizeof expected)) return -16;
 for(unsigned j=0;j<3;j++) {
  unsigned n=cp->l2_count+j,sec=(AA_BASE>>20)+j;
  if(s->sections[n]!=sec || s->l1[sec]!=((s->l2_pa+n*1024u)|1u)) return -16;
  for(unsigned p=0;p<256;p++) {
   unsigned page=j*256+p;
   uint32_t want=page<AA_BYTES/4096u?((U_POOL_PA+cp->used+page*4096u)|0x3fu):0;
   if(s->l2[n][p]!=want) return -16;
  }
 }
 return 0;
}
int aa_finish(struct aa_owner *o,struct aa_space *s,const struct aa_checkpoint *cp,bool publish) {
 int e=aa_validate_finish(o,s,cp);if(e) return e;
 const struct uregion expected={AA_BASE,AA_BYTES,U_POOL_PA+cp->used,U_R|U_W};
 if(publish) {
  o->pa=expected.pa;o->bytes=AA_BYTES;o->state=AA_RUNNING|AA_MAPPED;
 } else {
  for(unsigned j=0;j<3;j++) s->l1[(AA_BASE>>20)+j]=0;
  memset(s->l2+cp->l2_count,0,3*sizeof s->l2[0]);
  memset(s->sections+cp->l2_count,0,3*sizeof s->sections[0]);
  memset(&s->regions[cp->region_count],0,sizeof s->regions[0]);
  memset(s->pool+cp->used,0,AA_BYTES);
  s->used=cp->used;s->l2_count=cp->l2_count;s->region_count=cp->region_count;o->state=cp->state;
 }
 return 0;
}
