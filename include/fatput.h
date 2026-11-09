/* SPDX-License-Identifier: MIT; development file install for USB "PUTF":
 * create or replace one short-named file in /OS, or in /GAMES/<DIR> (a
 * content folder), on the boot card's FAT32 volume. Nothing else is ever
 * changed: no boot, firmware, slot or control file, no root entry except the
 * ones that create /OS or /GAMES, no directory entry outside those folders. */
#pragma once
#include <stdint.h>
#define FATPUT_MAX (1024u*1024u) /* the ELF loader's file limit */
extern const char *fatput_fail;
/* 0 if name is a plain 8.3 name (A-Z 0-9 _ -, upper case), else -EINVAL. */
int fatput_name(const char *name,uint8_t out[11]);
/* 0 if path is a name fatput accepts (see below), else -EINVAL. */
int fatput_check(const char *path);
/* path is "NAME" (/OS/NAME), "GAMES/DIR/NAME" or "APPS/DIR/NAME"
   (/OS/APPS/DIR/NAME; DIR an 8.3 name with no extension); missing folders are created. Write data, then the FAT chain,
   then the directory entry, then free the replaced chain. Every written
   sector is read back. 0 or a negative errno. */
int fatput(const char *path,const uint8_t *data,uint32_t bytes);
/* ABI 1.4 backend: plain 8.3 names in /OS/CACHE only, not USB paths. */
int fatcache_put(const char *name,const uint8_t *data,uint32_t bytes);
int fatcache_remove(const char *name);
int fatcache_invalidate(void); /* unpublish before install; bad cache data cannot deny installs */
int fatcache_rename(const char *from,const char *to,uint8_t *scratch,uint32_t cap);
#define FATSAVE_MAX_GAMES 64u /* folders under /OS/SAVES */
#define FATSAVE_MAX_FILES 32u /* files in one game's folder */
#define FATSAVE_EQUOTA 122    /* returned as -122 when a cap is hit */
/* Saves backend: plain 8.3 names in /OS/SAVES/<GAME> only (GAME has no dot). */
int fatsave_put(const char *game,const char *name,const uint8_t *data,uint32_t bytes);
int fatsave_remove(const char *game,const char *name);
int fatsave_rename(const char *game,const char *from,const char *to,uint8_t *scratch,uint32_t cap);
