/* SPDX-License-Identifier: MIT; record header rules from docs/design.md
 * (ABI draft 1.0): size equals the register size, meets the minimum, zero
 * flags and reserved words, no unknown nonzero trailing bytes. */
#include "kernel.h"
#include "abi.h"
int abi_check(const void *record,uint32_t bytes,uint32_t major,uint32_t min,uint32_t known) {
 if(major!=K_ABI_MAJOR) return -93; /* EPROTONOSUPPORT */
 if(!record||((uintptr_t)record&7)) return -14; /* EFAULT; records are 8-byte aligned */
 const struct k_header *h=record;
 if(bytes<sizeof(struct k_header)||bytes>4096||h->size!=bytes||bytes<min||bytes%8) return -22;
 if(h->flags||h->reserved0||h->reserved1) return -22;
 for(uint32_t i=known;i<bytes;i++) if(((const uint8_t *)record)[i]) return -22;
 return 0;
}
