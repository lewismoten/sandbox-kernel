/* SPDX-License-Identifier: MIT
 * Original unlinked slice A by Lewis Moten, with Hermes Agent / OpenAI Codex
 * GPT-6.1 Sol (openai-codex). Not a live ABI or a uspace binding.
 * All operations require one privileged serialized owner, no callbacks/service_io
 * during transactions, stable backing arrays and no user execution until finish.
 */
#pragma once
#include "uspace.h"
#define AA_BASE 0x30000000u
#define AA_BYTES 2195456u
#define AA_FLOOR (3u<<20)
#define AA_FB_BYTES 1228800u
#define AA_IDLE 0u
#define AA_LAUNCHING 1u
#define AA_RUNNING 2u
#define AA_QUIESCING 3u
#define AA_MAPPED 4u
#define AA_PENDING 8u
/* State is phase in low two bits plus lease flags. reserved stays zero. */
struct aa_owner {uint64_t last,active; uint32_t pa,bytes,state,reserved;};
struct aa_checkpoint {uint32_t used,l2_count,region_count,state;};
/* Storage record for later supervisor integration, not wired in this slice. */
struct aa_cleanup {
 uint32_t cursor,extent,disposition,error;
 char restart[32]; uint64_t decision_time; int64_t exit_status;
};
/* A borrowed view of the existing pool/tables; no second pool or new limits.
 * l2_pa is the checked physical base of the contiguous existing L2 array.
 * Counts/section IDs have the same meanings as uspace.c's private counters.
 * Later integration must explicitly transfer exclusive ownership; none exists now.
 */
struct aa_space {
 uint8_t *pool; uint32_t *l1; uint32_t (*l2)[256];
 struct uregion *regions; uint32_t *sections;
 uint32_t used,l2_count,region_count,l2_pa;
};
int aa_begin(struct aa_owner *o);
int aa_run(struct aa_owner *o);
int aa_preflight(const struct aa_space *s);
int aa_prepare(struct aa_owner *o,struct aa_space *s,uint64_t epoch,
               uint64_t requested,struct aa_checkpoint *cp);
/* publish=false models a late error BEFORE the first caller output store.
 * A changed append frontier is EBUSY, never rewind another mapping.
 * Success consumes PENDING; idempotent MAPPED prepare returns 1, no finish.
 */
int aa_finish(struct aa_owner *o,struct aa_space *s,
              const struct aa_checkpoint *cp,bool publish);
/* Read-only frontier validation before any caller output store. Finish repeats
 * it defensively; neither operation may call out or admit another mapper. */
int aa_validate_finish(const struct aa_owner *o,const struct aa_space *s,
                       const struct aa_checkpoint *cp);
int aa_quiesce(struct aa_owner *o,const struct aa_space *s,struct aa_cleanup *c);
/* One <=4096-byte scrub step. 1=done, 0=more, negative=quarantined failure.
 * Only after completion may backing storage be reused or a new launch begin.
 */
int aa_clean_step(struct aa_owner *o,struct aa_space *s,struct aa_cleanup *c);
_Static_assert(sizeof(struct aa_owner)==32,"owner budget");
_Static_assert(sizeof(struct aa_checkpoint)==16,"checkpoint budget");
_Static_assert(sizeof(struct aa_cleanup)==64,"cleanup budget");
_Static_assert(AA_BASE%4096==0 && AA_BYTES%4096==0,"page aligned");
_Static_assert((uint64_t)AA_BASE+AA_BYTES==0x30218000ull,"fixed interval");
