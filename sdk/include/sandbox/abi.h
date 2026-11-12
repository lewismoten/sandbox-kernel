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
#define SBX_ABI_MINOR 12u /* 1.12: power_hold operation 2 reports what is asleep (the app stops its sound); 1.11: power holds/timeouts; 1.10: /dev/serial0 (class 6); 1.9: /dev/eink e-ink title and page; 1.8: /dev/audio; 1.7: /OS/SAVES/<GAME>/ write scope; 1.6: opt-in generation-bound app arena */

/* Call numbers (r7 on ARM32). Only the calls with records below are implemented. */
enum {
  SBX_CALL_QUERY = 0, SBX_CALL_OPEN = 1, SBX_CALL_READ = 2, SBX_CALL_WRITE = 3,
  SBX_CALL_CLOSE = 4, SBX_CALL_IOCTL = 5, SBX_CALL_MMAP = 6, SBX_CALL_SEEK = 9, SBX_CALL_GETDENTS = 11,
  SBX_CALL_FSYNC = 12, SBX_CALL_CLOCK_GETTIME = 13, SBX_CALL_SLEEP_UNTIL = 14, SBX_CALL_EXIT = 16, SBX_CALL_HEALTH = 17,
  SBX_CALL_GETDENTS_META = 18, SBX_CALL_RENAME = 19,
  SBX_CALL_GETDENTS_LONG_META = 20, SBX_CALL_APP_ARENA = 21, SBX_CALL_POWER_HOLD = 22, SBX_CALL_POWER_TIMEOUTS = 23
};

/* Negative return values. */
enum {
  SBX_EPERM = 1, SBX_ENOENT = 2, SBX_EIO = 5, SBX_EBADF = 9, SBX_EAGAIN = 11,
  SBX_ENOMEM = 12, SBX_EACCES = 13, SBX_EFAULT = 14, SBX_EBUSY = 16,
  SBX_EEXIST = 17, SBX_ENODEV = 19, SBX_ENOTDIR = 20, SBX_EINVAL = 22,
  SBX_EMFILE = 24, SBX_ENOTTY = 25, SBX_ENOSPC = 28, SBX_ESPIPE = 29,
  SBX_EROFS = 30, SBX_EPIPE = 32, SBX_ENAMETOOLONG = 36, SBX_ENOSYS = 38,
  SBX_EOVERFLOW = 75, SBX_EPROTONOSUPPORT = 93, SBX_ETIMEDOUT = 110,
  SBX_EDQUOT = 122 /* 1.7: save folder or file count limit */
};

#define SBX_O_RDONLY 0u
#define SBX_O_WRONLY 1u
#define SBX_O_RDWR 2u
#define SBX_O_NONBLOCK 0x100u
#define SBX_O_CREAT 0x200u /* only WRONLY|CREAT|TRUNC, canonical /sd/OS/CACHE/NAME or /sd/OS/SAVES/GAME/NAME */
#define SBX_O_TRUNC 0x400u
#define SBX_MAX_TRANSFER (1024u * 1024u)
#define SBX_MAX_PATH 255u
#define SBX_SEEK_SET 0u /* ABI 1.3: only absolute seeking is supported */

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
#define SBX_IOC_FB_SET_MODE SBX_IOC(1, 2) /* 1.2: sbx_fb_set_rec; the firmware scales the mode to HDMI */
#define SBX_IOC_FB_PALETTE SBX_IOC(1, 3)  /* 1.2: sbx_fb_palette_rec, for SBX_FB_INDEX8 */
/* 1.8: open "/dev/audio" write-only for the one writer (a second open is EBUSY).
   write() takes whole stereo frames of signed 16-bit little-endian PCM (L,R,L,R...)
   at SBX_AUDIO_RATE, takes what fits and returns that many bytes, or EAGAIN when
   none fit (wait a little, retry). It never blocks and never overwrites queued audio.
   When the ring runs dry the device plays silence and counts an underrun. close()
   drops whatever is still queued. Check sbx_startup.minor >= 8 first. */
#define SBX_IOC_AUDIO_QUERY SBX_IOC(2, 0)
#define SBX_AUDIO_RATE 48000u
#define SBX_FB_XRGB8888 1u /* sbx_fb_mode.format: 32-bit words 0x00RRGGBB */
#define SBX_FB_RGB565 2u   /* 16-bit words rrrrrggggggbbbbb */
#define SBX_FB_INDEX8 3u   /* bytes, colors from the palette */
#define SBX_FB_FORMAT_XRGB8888 1u /* 32-bit little-endian words 0x00RRGGBB */
/* Class 4 (ABI 1.9) is the e-ink panel, /dev/eink, opened write-only: docs/eink.md. A page is
   written with write() as exactly SBX_EINK_PAGE_BYTES (250x122, 32-byte rows, bit 7 leftmost, 1 = black). */
#define SBX_IOC_EINK_QUERY SBX_IOC(4, 0)
#define SBX_IOC_EINK_TITLE SBX_IOC(4, 1) /* sbx_eink_title_rec */
#define SBX_IOC_EINK_CLEAR SBX_IOC(4, 2) /* a bare sbx_header: back to the kernel's screen */
#define SBX_EINK_WIDTH 250u
#define SBX_EINK_HEIGHT 122u
#define SBX_EINK_PAGE_BYTES 3904u
#define SBX_EINK_TITLE_MAX 16u
#define SBX_EINK_BUILD_MAX 16u /* the build line: what fits the status line at 2x */
#define SBX_EINK_MARK_BYTES 200u /* 40x40, 5-byte rows, same bit order */
#define SBX_EINK_TITLE_MARK 1u /* sbx_eink_title_rec.flags: a mark follows */
/* Class 3 is input (/dev/input/event0, opened read-only and nonblocking). */
#define SBX_IOC_INPUT_QUERY SBX_IOC(3, 0)
#define SBX_IOC_INPUT_STATE SBX_IOC(3, 1) /* current state; also empties the queue */
#define SBX_INPUT_SYNTHETIC 1u /* query flag: a synthetic source (USB KEY/AXIS commands) */
/* Event types and codes follow the Linux input conventions (event-codes.rst). */
#define SBX_EV_SYN 0u
#define SBX_EV_KEY 1u
#define SBX_EV_ABS 3u
#define SBX_SYN_REPORT 0u
#define SBX_SYN_DROPPED 3u /* events were lost: read the state, then go on */
/* Class 6 is the serial channel (1.10): /dev/serial0, nonblocking, one owner, a byte stream to the
   host on the USB link. See docs/serial.md. Bytes that do not fit, or that have no reader, are dropped and counted. */
#define SBX_IOC_SERIAL_QUERY SBX_IOC(6, 0)
#define SBX_SERIAL_NONE 0u /* sbx_serial_info.transport: no host has the port open */
#define SBX_SERIAL_USB 1u  /* the USB CDC link */
#define SBX_KEY_ESC 1u
#define SBX_KEY_Q 16u
#define SBX_KEY_ENTER 28u
#define SBX_KEY_SPACE 57u
#define SBX_KEY_UP 103u
#define SBX_KEY_LEFT 105u
#define SBX_KEY_RIGHT 106u
#define SBX_KEY_DOWN 108u
#define SBX_INPUT_KEYS 768u
#define SBX_INPUT_AXES 32u
/* getdents entry types */
#define SBX_DT_FILE 1u
#define SBX_DT_DIR 2u

typedef struct sbx_header {
  uint32_t size, flags, reserved0, reserved1;
} sbx_header;
#define SBX_POWER_NO_BLANK 1u
#define SBX_POWER_NO_DIM SBX_POWER_NO_BLANK
#define SBX_POWER_NO_SLOWDOWN 2u
typedef struct {sbx_header h; uint64_t operation,flags,token;} sbx_power_hold_rec;
typedef struct {sbx_header h; uint64_t slowdown_seconds,blank_seconds;} sbx_power_timeouts_rec;
_Static_assert(sizeof(sbx_power_hold_rec)==40,"power hold");
_Static_assert(sizeof(sbx_power_timeouts_rec)==32,"power timeouts");


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
typedef struct sbx_rename_rec {
  sbx_header h;
  uint64_t from, from_bytes, to, to_bytes;
} sbx_rename_rec;

typedef struct sbx_seek_rec { /* ABI 1.3: regular files only; offset in [0,size] */
  sbx_header h;
  uint64_t fd;
  int64_t offset;
  uint64_t whence; /* SBX_SEEK_SET only */
  uint64_t position; /* out; zero on entry; failures leave file position unchanged */
} sbx_seek_rec;

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

typedef struct sbx_fb_set_rec { /* SBX_IOC_FB_SET_MODE: width (a multiple of 4) x height x format, inside the surface */
  sbx_header h;
  uint64_t width, height, format;
} sbx_fb_set_rec;

typedef struct sbx_fb_palette_rec { /* SBX_IOC_FB_PALETTE: count entries from first, 0x00RRGGBB */
  sbx_header h;
  uint64_t first, count;
  uint32_t rgb[256];
} sbx_fb_palette_rec;

typedef struct sbx_mmap_rec {
  sbx_header h;
  uint64_t fd, offset, length, protection, flags;
  uint64_t address; /* out */
} sbx_mmap_rec;
/* Fixed singleton; requested is a minimum, not the charged size. No free/resize.
   Outputs are zero on entry. Epoch is NOT the boot-health generation. */
#define SBX_APP_ARENA_ADDRESS 0x30000000u
#define SBX_APP_ARENA_BYTES 2195456u
typedef struct sbx_app_arena_rec {
  sbx_header h;
  uint64_t owner_generation, requested_bytes, address, actual_bytes;
} sbx_app_arena_rec;

typedef struct sbx_dirents_rec { /* getdents: whole sbx_dirent entries into buffer */
  sbx_header h;
  uint64_t fd, buffer, bytes;
  uint64_t transferred; /* out; zero means no more entries */
} sbx_dirents_rec;
typedef struct sbx_dirent { /* followed by name_bytes of name (no NUL), padded so length is a multiple of 8 */
  uint16_t length;
  uint8_t type, name_bytes; /* SBX_DT_* */
  uint32_t size; /* file bytes; 0 for a directory */
} sbx_dirent;
/* ABI 1.4 call 18: short aliases. ABI 1.5 call 20: validated VFAT UTF-8 names
   (short alias on invalid/absent/oversized LFN). Same record and padding.
   Legacy calls 11 and 18 remain unchanged; call 20 needs a 280-byte buffer
   to fit any one entry. name_bytes counts bytes, not Unicode characters.
   FAT date:time words are opaque fingerprints, not Unix timestamps.
   identity is the first cluster, useful but not a content hash. */
typedef struct sbx_dirent_meta {
  uint16_t length;
  uint8_t type, name_bytes;
  uint32_t size, modified, created, identity, reserved;
} sbx_dirent_meta;
typedef struct sbx_health_rec { /* heartbeat; the first one is the readiness report */
  sbx_header h;
  uint64_t generation; /* sbx_startup.generation */
  uint64_t sequence;   /* strictly increasing, from 1 */
} sbx_health_rec;
#define SBX_CLOCK_MONOTONIC 0u /* nanoseconds since the kernel started (the input event clock) */
typedef struct sbx_clock_rec { /* clock_gettime (ABI 1.1) */
  sbx_header h;
  uint64_t clock; /* SBX_CLOCK_MONOTONIC */
  uint64_t ns;    /* out */
} sbx_clock_rec;
typedef struct sbx_sleep_rec { /* sleep_until (ABI 1.1): not run again before the deadline */
  sbx_header h;
  uint64_t deadline; /* absolute monotonic nanoseconds; one already passed returns at once */
} sbx_sleep_rec;
typedef struct sbx_eink_info { /* SBX_IOC_EINK_QUERY output (fields zero on entry) */
  sbx_header h;
  uint64_t width, height, page_bytes, title_max, mark_bytes, min_interval_ms, max_per_minute, flags;
} sbx_eink_info;
typedef struct sbx_eink_title_rec { /* name 1-16 chars (kernel font, capitals), optional mark */
  sbx_header h;
  uint64_t flags, name_bytes;
  char name[16];
  uint8_t mark[200];
  uint64_t build_bytes; /* 0-16: the build line under the name; 0 = blank */
  char build[16];       /* free text: a hash, a version, a date; the kernel font, capitals; anything else shows as ? */
} sbx_eink_title_rec;
typedef struct sbx_input_event { /* 16 bytes; read returns whole records */
  uint64_t ns; /* monotonic nanoseconds since the kernel started */
  uint16_t type, code;
  int32_t value; /* key: 0 up, 1 down, 2 repeat; axis: position */
} sbx_input_event;
typedef struct sbx_audio_info { /* SBX_IOC_AUDIO_QUERY output; counters are for this open */
  sbx_header h;
  uint64_t flags, rate, capacity_frames, queued_frames, played_frames;
  uint64_t underruns;      /* drains that found the ring short */
  uint64_t overruns;       /* writes that did not fit whole */
  uint64_t overrun_frames; /* frames those writes were refused */
} sbx_audio_info;
typedef struct sbx_input_info { /* SBX_IOC_INPUT_QUERY output */
  sbx_header h;
  uint64_t flags, instance, queue_records, keys, axes;
  int64_t axis_min, axis_max;
} sbx_input_info;
typedef struct sbx_input_state { /* SBX_IOC_INPUT_STATE output */
  sbx_header h;
  uint64_t keys[SBX_INPUT_KEYS / 64]; /* bit (code % 64) of word (code / 64) is set while down */
  int64_t axes[SBX_INPUT_AXES];
} sbx_input_state;
typedef struct sbx_serial_info { /* SBX_IOC_SERIAL_QUERY output (1.10) */
  sbx_header h;
  uint64_t transport, tx_bytes, tx_dropped, rx_bytes, rx_dropped, tx_pending, rx_pending;
} sbx_serial_info;
/* Read-only record whose address is in r0 at entry. Returning from the entry
   point ends the program with r0 as its exit status. */
typedef struct sbx_startup {
  uint32_t size;
  uint16_t major, minor;
  uint32_t generation, trial; /* kernel boot generation; 1 while it is on trial */
  uint64_t image_bytes, stack_bytes, handles; /* quotas */
} sbx_startup;
typedef struct sbx_startup_ext {
  sbx_startup prefix;
  uint64_t app_generation;
} sbx_startup_ext;
/* Short legacy records are valid: never access the extension before all gates. */
static inline uint64_t sbx_app_generation(const sbx_startup *s) {
  if (!s || s->major != 1 || s->minor < 6 || s->size < sizeof(sbx_startup_ext)) return 0;
  return ((const sbx_startup_ext *)(const void *)s)->app_generation;
}

#define SBX_CHECK(type, bytes) _Static_assert(sizeof(type) == (bytes), #type " size")
#define SBX_AT(type, field, at) _Static_assert(offsetof(type, field) == (at), #type "." #field)
SBX_CHECK(sbx_header, 16);
SBX_CHECK(sbx_eink_info, 80); SBX_CHECK(sbx_eink_title_rec, 272); SBX_AT(sbx_eink_title_rec, name, 32); SBX_AT(sbx_eink_title_rec, mark, 48);
SBX_CHECK(sbx_startup_ext, 48); SBX_AT(sbx_startup_ext, prefix, 0); SBX_AT(sbx_startup_ext, app_generation, 40);
SBX_CHECK(sbx_app_arena_rec, 48); SBX_AT(sbx_app_arena_rec, h, 0);
SBX_AT(sbx_app_arena_rec, owner_generation, 16); SBX_AT(sbx_app_arena_rec, requested_bytes, 24);
SBX_AT(sbx_app_arena_rec, address, 32); SBX_AT(sbx_app_arena_rec, actual_bytes, 40);
SBX_AT(sbx_header, size, 0); SBX_AT(sbx_header, flags, 4);
SBX_AT(sbx_header, reserved0, 8); SBX_AT(sbx_header, reserved1, 12);
SBX_CHECK(sbx_open_rec, 56);
SBX_AT(sbx_open_rec, path, 16); SBX_AT(sbx_open_rec, path_bytes, 24);
SBX_AT(sbx_open_rec, flags, 32); SBX_AT(sbx_open_rec, mode, 40); SBX_AT(sbx_open_rec, fd, 48);
SBX_CHECK(sbx_io_rec, 48);
SBX_AT(sbx_io_rec, fd, 16); SBX_AT(sbx_io_rec, buffer, 24);
SBX_AT(sbx_io_rec, count, 32); SBX_AT(sbx_io_rec, transferred, 40);
SBX_CHECK(sbx_close_rec, 24); SBX_AT(sbx_close_rec, fd, 16);
SBX_CHECK(sbx_rename_rec, 48); SBX_AT(sbx_rename_rec, from, 16); SBX_AT(sbx_rename_rec, to_bytes, 40);
SBX_CHECK(sbx_dirent_meta, 24); SBX_AT(sbx_dirent_meta, modified, 8); SBX_AT(sbx_dirent_meta, reserved, 20);
SBX_CHECK(sbx_seek_rec, 48); SBX_AT(sbx_seek_rec, fd, 16); SBX_AT(sbx_seek_rec, offset, 24);
SBX_AT(sbx_seek_rec, whence, 32); SBX_AT(sbx_seek_rec, position, 40);
SBX_CHECK(sbx_ioctl_rec, 48);
SBX_AT(sbx_ioctl_rec, fd, 16); SBX_AT(sbx_ioctl_rec, request, 24);
SBX_AT(sbx_ioctl_rec, buffer, 32); SBX_AT(sbx_ioctl_rec, bytes, 40);
SBX_CHECK(sbx_exit_rec, 24); SBX_AT(sbx_exit_rec, status, 16);
SBX_CHECK(sbx_fb_mode, 56);
SBX_AT(sbx_fb_mode, width, 16); SBX_AT(sbx_fb_mode, height, 24); SBX_AT(sbx_fb_mode, pitch, 32);
SBX_AT(sbx_fb_mode, format, 40); SBX_AT(sbx_fb_mode, buffer_bytes, 48);
SBX_CHECK(sbx_fb_present_rec, 48);
SBX_CHECK(sbx_fb_set_rec, 40);
SBX_CHECK(sbx_fb_palette_rec, 1056);
SBX_AT(sbx_fb_present_rec, x, 16); SBX_AT(sbx_fb_present_rec, y, 24);
SBX_AT(sbx_fb_present_rec, width, 32); SBX_AT(sbx_fb_present_rec, height, 40);
SBX_CHECK(sbx_mmap_rec, 64);
SBX_AT(sbx_mmap_rec, fd, 16); SBX_AT(sbx_mmap_rec, offset, 24); SBX_AT(sbx_mmap_rec, length, 32);
SBX_AT(sbx_mmap_rec, protection, 40); SBX_AT(sbx_mmap_rec, flags, 48); SBX_AT(sbx_mmap_rec, address, 56);
SBX_CHECK(sbx_startup, 40);
SBX_AT(sbx_startup, major, 4); SBX_AT(sbx_startup, minor, 6); SBX_AT(sbx_startup, generation, 8);
SBX_AT(sbx_startup, trial, 12); SBX_AT(sbx_startup, image_bytes, 16); SBX_AT(sbx_startup, stack_bytes, 24);
SBX_AT(sbx_startup, handles, 32);
SBX_CHECK(sbx_dirents_rec, 48);
SBX_AT(sbx_dirents_rec, fd, 16); SBX_AT(sbx_dirents_rec, buffer, 24); SBX_AT(sbx_dirents_rec, bytes, 32); SBX_AT(sbx_dirents_rec, transferred, 40);
SBX_CHECK(sbx_dirent, 8); SBX_AT(sbx_dirent, type, 2); SBX_AT(sbx_dirent, name_bytes, 3); SBX_AT(sbx_dirent, size, 4);
SBX_CHECK(sbx_health_rec, 32); SBX_AT(sbx_health_rec, generation, 16); SBX_AT(sbx_health_rec, sequence, 24);
SBX_CHECK(sbx_clock_rec, 32); SBX_AT(sbx_clock_rec, clock, 16); SBX_AT(sbx_clock_rec, ns, 24);
SBX_CHECK(sbx_sleep_rec, 24); SBX_AT(sbx_sleep_rec, deadline, 16);
SBX_CHECK(sbx_input_event, 16); SBX_AT(sbx_input_event, type, 8); SBX_AT(sbx_input_event, code, 10); SBX_AT(sbx_input_event, value, 12);
SBX_CHECK(sbx_input_info, 72);
SBX_AT(sbx_input_info, flags, 16); SBX_AT(sbx_input_info, instance, 24); SBX_AT(sbx_input_info, queue_records, 32);
SBX_AT(sbx_input_info, keys, 40); SBX_AT(sbx_input_info, axes, 48); SBX_AT(sbx_input_info, axis_min, 56); SBX_AT(sbx_input_info, axis_max, 64);
SBX_CHECK(sbx_serial_info, 72); SBX_AT(sbx_serial_info, transport, 16); SBX_AT(sbx_serial_info, rx_pending, 64);
SBX_CHECK(sbx_input_state, 368); SBX_AT(sbx_input_state, keys, 16); SBX_AT(sbx_input_state, axes, 112);
#undef SBX_CHECK
#undef SBX_AT
#endif
