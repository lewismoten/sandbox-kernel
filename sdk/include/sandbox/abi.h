/* SPDX-License-Identifier: MIT
 * sandbox-kernel SDK: system-call ABI, draft 1.0 (docs/design.md).
 * Fixed little-endian record layouts. Every record starts with a 16-byte
 * header; payload fields are 8-byte slots; addresses and lengths are u64 even
 * on ARM32 (upper bits zero there). No compiler packing: the asserts below
 * pin every size and offset, and the kernel checks the same numbers. */
#ifndef SANDBOX_ABI_H
#define SANDBOX_ABI_H
#include <stdint.h>
#include <stddef.h>

#define SBX_ABI_MAJOR 1u
#define SBX_ABI_MINOR 0u

/* Call numbers (r7 on ARM32). Only the first calls have records here. */
enum {
  SBX_CALL_QUERY = 0, SBX_CALL_OPEN = 1, SBX_CALL_READ = 2, SBX_CALL_WRITE = 3,
  SBX_CALL_CLOSE = 4, SBX_CALL_IOCTL = 5, SBX_CALL_MMAP = 6, SBX_CALL_EXIT = 16
};

/* Negative return values. */
enum {
  SBX_EPERM = 1, SBX_ENOENT = 2, SBX_EIO = 5, SBX_EBADF = 9, SBX_EAGAIN = 11,
  SBX_ENOMEM = 12, SBX_EACCES = 13, SBX_EFAULT = 14, SBX_EBUSY = 16,
  SBX_EEXIST = 17, SBX_ENODEV = 19, SBX_ENOTDIR = 20, SBX_EINVAL = 22,
  SBX_EMFILE = 24, SBX_ENOTTY = 25, SBX_ENOSPC = 28, SBX_ESPIPE = 29,
  SBX_EROFS = 30, SBX_EPIPE = 32, SBX_ENAMETOOLONG = 36, SBX_ENOSYS = 38,
  SBX_EPROTONOSUPPORT = 93, SBX_ETIMEDOUT = 110
};

#define SBX_O_RDONLY 0u
#define SBX_O_WRONLY 1u
#define SBX_O_RDWR 2u
#define SBX_O_NONBLOCK 0x100u
#define SBX_MAX_TRANSFER (1024u * 1024u)
#define SBX_MAX_PATH 255u

/* mmap protections and flags. Only an approved shared buffer (the /dev/fb0
   surface) can be mapped in this draft; the kernel chooses the address. */
#define SBX_PROT_READ 1u
#define SBX_PROT_WRITE 2u
#define SBX_PROT_EXEC 4u
#define SBX_MAP_SHARED 1u
#define SBX_MAP_PRIVATE 2u
#define SBX_MAP_ANONYMOUS 0x20u

/* ioctl request = (class << 16) | operation. Class 1 is the framebuffer. */
#define SBX_IOC(class_, op) ((uint32_t)(class_) << 16 | (uint32_t)(op))
#define SBX_IOC_FB_QUERY SBX_IOC(1, 0)
#define SBX_IOC_FB_PRESENT SBX_IOC(1, 1)
#define SBX_FB_FORMAT_XRGB8888 1u /* 32-bit little-endian words 0x00RRGGBB */

typedef struct sbx_header {
  uint32_t size, flags, reserved0, reserved1;
} sbx_header;

typedef struct sbx_open_rec { /* path is absolute UTF-8, no NUL inside */
  sbx_header h;
  uint64_t path, path_bytes, flags, mode;
  uint64_t fd; /* out */
} sbx_open_rec;

typedef struct sbx_io_rec { /* read and write; /dev/log writes are log lines */
  sbx_header h;
  uint64_t fd, buffer, count;
  uint64_t transferred; /* out; zero on read means end of file */
} sbx_io_rec;

typedef struct sbx_close_rec {
  sbx_header h;
  uint64_t fd;
} sbx_close_rec;

typedef struct sbx_ioctl_rec { /* buffer points to a class record below */
  sbx_header h;
  uint64_t fd, request, buffer, bytes;
} sbx_ioctl_rec;

typedef struct sbx_exit_rec {
  sbx_header h;
  int64_t status;
} sbx_exit_rec;

typedef struct sbx_fb_mode { /* SBX_IOC_FB_QUERY output on /dev/fb0 */
  sbx_header h;
  uint64_t width, height, pitch, format, buffer_bytes;
} sbx_fb_mode;

typedef struct sbx_fb_present_rec { /* copy this rectangle of the mapped surface to the screen */
  sbx_header h;
  uint64_t x, y, width, height;
} sbx_fb_present_rec;

typedef struct sbx_mmap_rec {
  sbx_header h;
  uint64_t fd, offset, length, protection, flags;
  uint64_t address; /* out */
} sbx_mmap_rec;

/* Read-only record whose address is in r0 at entry. Returning from the entry
   point ends the program with r0 as its exit status. */
typedef struct sbx_startup {
  uint32_t size;
  uint16_t major, minor;
  uint32_t generation, trial; /* kernel boot generation; 1 while it is on trial */
  uint64_t image_bytes, stack_bytes, handles; /* quotas */
} sbx_startup;

#define SBX_CHECK(type, bytes) _Static_assert(sizeof(type) == (bytes), #type " size")
#define SBX_AT(type, field, at) _Static_assert(offsetof(type, field) == (at), #type "." #field)
SBX_CHECK(sbx_header, 16);
SBX_AT(sbx_header, size, 0); SBX_AT(sbx_header, flags, 4);
SBX_AT(sbx_header, reserved0, 8); SBX_AT(sbx_header, reserved1, 12);
SBX_CHECK(sbx_open_rec, 56);
SBX_AT(sbx_open_rec, path, 16); SBX_AT(sbx_open_rec, path_bytes, 24);
SBX_AT(sbx_open_rec, flags, 32); SBX_AT(sbx_open_rec, mode, 40); SBX_AT(sbx_open_rec, fd, 48);
SBX_CHECK(sbx_io_rec, 48);
SBX_AT(sbx_io_rec, fd, 16); SBX_AT(sbx_io_rec, buffer, 24);
SBX_AT(sbx_io_rec, count, 32); SBX_AT(sbx_io_rec, transferred, 40);
SBX_CHECK(sbx_close_rec, 24); SBX_AT(sbx_close_rec, fd, 16);
SBX_CHECK(sbx_ioctl_rec, 48);
SBX_AT(sbx_ioctl_rec, fd, 16); SBX_AT(sbx_ioctl_rec, request, 24);
SBX_AT(sbx_ioctl_rec, buffer, 32); SBX_AT(sbx_ioctl_rec, bytes, 40);
SBX_CHECK(sbx_exit_rec, 24); SBX_AT(sbx_exit_rec, status, 16);
SBX_CHECK(sbx_fb_mode, 56);
SBX_AT(sbx_fb_mode, width, 16); SBX_AT(sbx_fb_mode, height, 24); SBX_AT(sbx_fb_mode, pitch, 32);
SBX_AT(sbx_fb_mode, format, 40); SBX_AT(sbx_fb_mode, buffer_bytes, 48);
SBX_CHECK(sbx_fb_present_rec, 48);
SBX_AT(sbx_fb_present_rec, x, 16); SBX_AT(sbx_fb_present_rec, y, 24);
SBX_AT(sbx_fb_present_rec, width, 32); SBX_AT(sbx_fb_present_rec, height, 40);
SBX_CHECK(sbx_mmap_rec, 64);
SBX_AT(sbx_mmap_rec, fd, 16); SBX_AT(sbx_mmap_rec, offset, 24); SBX_AT(sbx_mmap_rec, length, 32);
SBX_AT(sbx_mmap_rec, protection, 40); SBX_AT(sbx_mmap_rec, flags, 48); SBX_AT(sbx_mmap_rec, address, 56);
SBX_CHECK(sbx_startup, 40);
SBX_AT(sbx_startup, major, 4); SBX_AT(sbx_startup, minor, 6); SBX_AT(sbx_startup, generation, 8);
SBX_AT(sbx_startup, trial, 12); SBX_AT(sbx_startup, image_bytes, 16); SBX_AT(sbx_startup, stack_bytes, 24);
SBX_AT(sbx_startup, handles, 32);
#undef SBX_CHECK
#undef SBX_AT
#endif
