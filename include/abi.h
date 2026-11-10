/* SPDX-License-Identifier: MIT; kernel-side declaration of the draft 1.0
 * system-call records. Written separately from sdk/include/sandbox/abi.h on
 * purpose: tests/abi_layout.c checks that both agree field by field. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define K_ABI_MAJOR 1u
#define K_ABI_MINOR 10u /* 1.10: /dev/serial0; 1.9: /dev/eink; 1.8: /dev/audio; 1.7: /OS/SAVES/<GAME>/ write scope */
struct k_header {uint32_t size,flags,reserved0,reserved1;};
struct k_open {struct k_header h; uint64_t path,path_bytes,flags,mode,fd;};
struct k_io {struct k_header h; uint64_t fd,buffer,count,transferred;};
struct k_close {struct k_header h; uint64_t fd;};
struct k_rename {struct k_header h; uint64_t from,from_bytes,to,to_bytes;};
struct k_seek {struct k_header h; uint64_t fd; int64_t offset; uint64_t whence,position;};
struct k_ioctl {struct k_header h; uint64_t fd,request,buffer,bytes;};
struct k_exit {struct k_header h; int64_t status;};
struct k_fb_mode {struct k_header h; uint64_t width,height,pitch,format,buffer_bytes;};
struct k_fb_present {struct k_header h; uint64_t x,y,width,height;};
struct k_fb_set {struct k_header h; uint64_t width,height,format;}; /* ABI 1.2: format 1 XRGB8888, 2 RGB565, 3 8-bit indices */
struct k_fb_palette {struct k_header h; uint64_t first,count; uint32_t rgb[256];}; /* ABI 1.2: 0x00RRGGBB */
struct k_mmap {struct k_header h; uint64_t fd,offset,length,protection,flags,address;};
struct k_app_arena {struct k_header h; uint64_t owner_generation,requested_bytes,address,actual_bytes;};
_Static_assert(sizeof(struct k_app_arena)==48,"arena record");
struct k_audio_info {struct k_header h; uint64_t flags,rate,capacity_frames,queued_frames,played_frames,underruns,overruns,overrun_frames;}; /* ABI 1.8 /dev/audio */
struct k_health {struct k_header h; uint64_t generation,sequence;};
struct k_clock {struct k_header h; uint64_t clock,ns;}; /* clock_gettime (13): clock 0 is monotonic; ns out */
struct k_sleep {struct k_header h; uint64_t deadline;}; /* sleep_until (14): absolute monotonic ns */
struct k_input_info {struct k_header h; uint64_t flags,instance,queue_records,keys,axes; int64_t axis_min,axis_max;};
struct k_input_state {struct k_header h; uint64_t keys[12]; int64_t axes[32];};
struct k_serial_info {struct k_header h; uint64_t transport,tx_bytes,tx_dropped,rx_bytes,rx_dropped,tx_pending,rx_pending;}; /* ABI 1.10 */
struct k_input_event {uint64_t ns; uint16_t type,code; int32_t value;};
struct k_eink_info {struct k_header h; uint64_t width,height,page_bytes,title_max,mark_bytes,min_interval_ms,max_per_minute,flags;}; /* ABI 1.9 */
struct k_eink_title {struct k_header h; uint64_t flags,name_bytes; char name[16]; uint8_t mark[200]; uint64_t build_bytes; char build[16];}; /* ABI 1.9: flags bit 0 = a mark; build_bytes 0-16 = the build line (free text), 0 = none */
struct k_dirent {uint16_t length; uint8_t type,name_bytes; uint32_t size;}; /* then the name, padded to 8 bytes */
struct k_dirent_meta {uint16_t length;uint8_t type,name_bytes;uint32_t size,modified,created,identity,reserved;};
struct k_startup {uint32_t size; uint16_t major,minor; uint32_t generation,trial; uint64_t image_bytes,stack_bytes,handles;};
struct k_startup_ext {struct k_startup prefix; uint64_t app_generation;};
_Static_assert(sizeof(struct k_startup_ext)==48,"startup extension");
_Static_assert(offsetof(struct k_startup_ext,app_generation)==40,"app epoch offset");
#define K_SIZE(t,n) _Static_assert(sizeof(struct t)==(n),#t)
K_SIZE(k_header,16); K_SIZE(k_open,56); K_SIZE(k_io,48); K_SIZE(k_close,24);
K_SIZE(k_seek,48);
K_SIZE(k_rename,48);K_SIZE(k_dirent_meta,24);
K_SIZE(k_ioctl,48); K_SIZE(k_exit,24); K_SIZE(k_fb_mode,56); K_SIZE(k_fb_present,48); K_SIZE(k_fb_set,40); K_SIZE(k_fb_palette,1056); K_SIZE(k_mmap,64); K_SIZE(k_startup,40);
K_SIZE(k_eink_info,80); K_SIZE(k_eink_title,272); K_SIZE(k_audio_info,80); K_SIZE(k_health,32); K_SIZE(k_clock,32); K_SIZE(k_sleep,24); K_SIZE(k_input_info,72); K_SIZE(k_input_state,368); K_SIZE(k_input_event,16); K_SIZE(k_serial_info,72); K_SIZE(k_dirent,8);
_Static_assert(offsetof(struct k_open,fd)==48&&offsetof(struct k_io,transferred)==40&&offsetof(struct k_fb_mode,buffer_bytes)==48,"tail fields");
#undef K_SIZE
/* Check a record header against the registers: 0 or a negative errno.
 * known = bytes this kernel understands; longer records must have zero tails. */
int abi_check(const void *record,uint32_t reg_bytes,uint32_t reg_major,uint32_t min_bytes,uint32_t known_bytes);
