/* SPDX-License-Identifier: MIT; read-only FAT32 asset API over the boot
 * card's first partition (later the /sd tree). Separate from the A/B control
 * store: it never writes. Returns are >= 0 on success or a negative ABI errno. */
#pragma once
#include <stdint.h>
#define ASSET_HANDLES 8u
#define ASSET_MAX_PATH 255u
#define ASSET_MAX_DEPTH 16u
#define ASSET_MAX_READ (1024u*1024u) /* ABI transfer limit */
#define ASSET_MAX_BYTES (64u*1024u*1024u) /* largest file opened in slice 1 */
#define ASSET_ENOENT 2
#define ASSET_EIO 5
#define ASSET_EBADF 9
#define ASSET_ENOTDIR 20
#define ASSET_EINVAL 22
#define ASSET_EMFILE 24
#define ASSET_EROFS 30
#define ASSET_ENAMETOOLONG 36
#define ASSET_ENODEV 19
struct asset_dirent { char name[13]; uint8_t attr; uint8_t is_dir; uint8_t reserved; uint32_t size; };
extern const char *asset_fail; /* short code of the last failure, for logs */
int asset_mount(void); /* parse MBR/BPB again; closes every handle */
int asset_open(const char *path,uint32_t bytes,uint32_t flags); /* handle */
int asset_read(int handle,void *buffer,uint32_t count); /* bytes; 0 is EOF */
int asset_readdir(int handle,struct asset_dirent *out); /* 1 entry, 0 end */
int asset_close(int handle);
int asset_size(int handle,uint32_t *bytes);
struct asset_volume {uint32_t base,end,fat,fat_size,data,clusters,spc,root;};
int asset_volume(struct asset_volume *out); /* geometry of the mounted volume, for core/fatput.c */
