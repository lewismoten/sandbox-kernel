/* SPDX-License-Identifier: MIT; development file install for USB "PUTF":
 * create or replace one short-named file in /OS, or in /GAMES/<DIR> (a
 * content folder), on the boot card's FAT32 volume. Nothing else is ever
 * changed: no boot, firmware, slot or control file, no root entry except the
 * ones that create /OS or /GAMES, no directory entry outside those folders. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
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
/* USB MV, RM, RMDIR and LS: real card paths, no leading slash, upper-case 8.3 parts (LS also takes "/").
   Changing commands reach only GAMES, POCKET and OS/APPS (any depth) and plain files in OS. Each returns 0 or a
   negative errno, with fatput_fail "MOV ..." for a refusal and "PUT ..." for a storage fault. */
int fatfs_mv(const char *from,const char *to);
int fatfs_rm(const char *path);
int fatfs_rmdir(const char *path);
/* ENT lines into out (NUL-terminated, under cap bytes) from raw slot `from`; *next resumes (~0u at the end);
   dot-named (macOS) entries are skipped and counted in *skipped. Returns the line count or a negative errno. */
int fatfs_ls(const char *path,uint32_t from,char *out,uint32_t cap,uint32_t *next,uint32_t *skipped);

/* Bounded in-place logs in /OS/SAVES/KERNEL (see fatput.c): the file is a fixed size, text then NUL padding. fatlog_set
   copies the text (no card access); fatlog_pump, called once per main-loop pass, creates the file if it is missing, then
   writes at most ONE changed 512-byte sector per call. Returns 1 for a sector written, 0 for nothing to do, <0 on error. */
#define FATLOG_CEC 0u
#define FATLOG_AUDIO 1u
#define FATLOG_SLOTS 2u
#define FATLOG_CEC_CAP 24576u
#define FATLOG_AUDIO_CAP 8704u /* 17 sectors: AUDIO_LOG_BYTES plus the header line */
#define FATLOG_MAXSEC (FATLOG_CEC_CAP/512u)
int fatlog_set(unsigned id,const uint8_t *data,uint32_t bytes);
int fatlog_pump(void);
bool fatlog_pending(unsigned id);
uint32_t fatlog_writes(void);
