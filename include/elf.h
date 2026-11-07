/* SPDX-License-Identifier: MIT; strict ELF32 checks for OS programs
 * (acceptance test 3): little-endian ELF32 ET_EXEC, EM_ARM, EABI5 soft-float,
 * ARM-state entry inside an executable segment, bounded non-overlapping
 * PT_LOAD segments in the user image window, never writable and executable.
 * elf_check is pure; elf_load copies into the user space (uspace.h). */
#pragma once
#include <stdint.h>
#define ELF_MAX_FILE (1u<<20)
#define ELF_MAX_PHDRS 16u
#define ELF_MAX_SEGMENTS 4u
struct elf_seg {uint32_t va,memsz,filesz,offset,prot;};
struct elf_plan {uint32_t entry,count,bytes; struct elf_seg seg[ELF_MAX_SEGMENTS];};
/* NULL if the file may be loaded, else a short reason code. */
const char *elf_check(const uint8_t *file,uint32_t bytes,struct elf_plan *plan);
/* Map and fill every segment (BSS zeroed). 0 or a negative errno. */
int elf_load(const uint8_t *file,const struct elf_plan *plan);
