/* SPDX-License-Identifier: MIT; the OS program's address space on ARMv6
 * (ARM1176, extended small-page descriptors). The privileged table is the
 * kernel's identity map; the user table is a copy of it, whose kernel and MMIO
 * entries stay privileged-only, plus 4 KiB user pages from a reserved pool.
 * User text is read-only and executable, user data writable and never
 * executable. The kernel reaches user memory only through us_span() checks,
 * never by dereferencing a user address. Pure code; host tested. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define U_IMAGE_BASE 0x08000000u /* fixed-address ELF segments */
#define U_IMAGE_END 0x10000000u
#define U_IMAGE_QUOTA (8u<<20)
#define U_FB_BASE 0x40000000u /* the /dev/fb0 surface, once mapped */
#define U_START_VA 0x6ffff000u /* read-only startup record */
#define U_STACK_TOP 0x7fff0000u /* stack grows down; the MiB below it is a guard */
#define U_STACK_BYTES (1u<<20)
#define U_EXIT_VA 0x0000f000u /* lr at entry: a return lands here and is an exit */
#define U_POOL_PA 0x04000000u /* physical pool for user pages, inside kernel RAM */
#define U_POOL_BYTES (16u<<20)
#define U_REGIONS 8u
#define U_L2 16u
#define U_R 4u /* ELF segment flag values */
#define U_W 2u
#define U_X 1u
struct uregion {uint32_t va,bytes,pa,prot;};
extern struct uregion us_regions[U_REGIONS];
extern unsigned us_count;
extern uint32_t us_table[4096]; /* user L1, 16 KiB aligned on the board */
extern uint32_t us_l2[U_L2][256];
extern const char *us_fail;
/* Copy the privileged L1 and drop every user mapping. -EINVAL if the kernel
   map already uses a user range (USER MAP CONFLICT). */
int us_reset(const uint32_t *kernel_l1);
/* Map zeroed pool pages at a page-aligned va. 0, -EINVAL, -ENOMEM. */
int us_map(uint32_t va,uint32_t bytes,uint32_t prot);
/* Bytes reachable contiguously from va (at most bytes) with the access in
   need (U_R or U_W), and a kernel pointer to them; -EFAULT if none. */
int64_t us_span(uint32_t va,uint32_t bytes,uint32_t need,uint8_t **ptr);
int us_check(uint64_t va,uint64_t bytes,uint32_t need); /* whole range: 0 or -EFAULT */
int us_copy_in(void *dst,uint64_t va,uint32_t bytes);
int us_copy_out(uint64_t va,const void *src,uint32_t bytes);
uint8_t *us_pool(void); /* kernel pointer to the pool's first byte */
uint32_t us_l2_pa(unsigned i); /* physical address of us_l2[i] */
uint32_t us_used(void); /* pool bytes in use */
struct aa_owner; struct aa_cleanup;
struct aa_checkpoint;
/* Exclusive append-only transaction; no callbacks/maps/user entry between
   prepare and finish. 1 means an existing lease, requiring no finish. */
int us_arena_prepare(struct aa_owner *owner,uint64_t epoch,uint64_t requested,struct aa_checkpoint *checkpoint);
int us_arena_finish(struct aa_owner *owner,const struct aa_checkpoint *checkpoint,bool publish);
int us_arena_validate(const struct aa_owner *owner,const struct aa_checkpoint *checkpoint);
/* Privileged main-loop only: borrow the canonical counters exclusively. No
   callbacks or user entry until cleanup completes; never reset before scrub. */
int us_quiesce(struct aa_owner *owner,struct aa_cleanup *cleanup);
int us_clean_step(struct aa_owner *owner,struct aa_cleanup *cleanup);
