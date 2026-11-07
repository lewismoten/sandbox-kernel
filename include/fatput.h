/* SPDX-License-Identifier: MIT; development file install for USB "PUTF":
 * create or replace one short-named file in /OS on the boot card's FAT32
 * volume. Nothing outside /OS is ever changed: no boot, firmware, slot or
 * control file, no root entry except the one that creates /OS. */
#pragma once
#include <stdint.h>
#define FATPUT_MAX (512u*1024u)
extern const char *fatput_fail;
/* 0 if name is a plain 8.3 name (A-Z 0-9 _ -, upper case), else -EINVAL. */
int fatput_name(const char *name,uint8_t out[11]);
/* Write data, then the FAT chain, then the directory entry, then free the
   replaced chain. Every written sector is read back. 0 or a negative errno. */
int fatput(const char *name,const uint8_t *data,uint32_t bytes);
