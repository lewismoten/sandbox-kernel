/* SPDX-License-Identifier: MIT; kernel-side declaration of the draft 1.0
 * system-call records. Written separately from sdk/include/sandbox/abi.h on
 * purpose: tests/abi_layout.c checks that both agree field by field. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define K_ABI_MAJOR 1u
#define K_ABI_MINOR 0u
struct k_header {uint32_t size,flags,reserved0,reserved1;};
struct k_open {struct k_header h; uint64_t path,path_bytes,flags,mode,fd;};
struct k_io {struct k_header h; uint64_t fd,buffer,count,transferred;};
struct k_close {struct k_header h; uint64_t fd;};
struct k_ioctl {struct k_header h; uint64_t fd,request,buffer,bytes;};
struct k_exit {struct k_header h; int64_t status;};
struct k_fb_mode {struct k_header h; uint64_t width,height,pitch,format,buffer_bytes;};
struct k_fb_present {struct k_header h; uint64_t x,y,width,height;};
#define K_SIZE(t,n) _Static_assert(sizeof(struct t)==(n),#t)
K_SIZE(k_header,16); K_SIZE(k_open,56); K_SIZE(k_io,48); K_SIZE(k_close,24);
K_SIZE(k_ioctl,48); K_SIZE(k_exit,24); K_SIZE(k_fb_mode,56); K_SIZE(k_fb_present,48);
_Static_assert(offsetof(struct k_open,fd)==48&&offsetof(struct k_io,transferred)==40&&offsetof(struct k_fb_mode,buffer_bytes)==48,"tail fields");
#undef K_SIZE
/* Check a record header against the registers: 0 or a negative errno.
 * known = bytes this kernel understands; longer records must have zero tails. */
int abi_check(const void *record,uint32_t reg_bytes,uint32_t reg_major,uint32_t min_bytes,uint32_t known_bytes);
