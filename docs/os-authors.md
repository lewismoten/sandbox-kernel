# Writing an operating system for sandbox-kernel

This guide is for someone who wants to make their own OS on the Raspberry Pi Zero W
using sandbox-kernel, in C or in JavaScript. It says what the kernel is, what happens
from power-on to your first instruction, what your program may rely on, and what it may
not. Everything here comes from the code and docs on `develop`; file and function names
are given so you can check. Anything planned but not built is marked **not yet**.

Contents: [1. What the kernel is](#1-what-the-kernel-is) |
[2. Boot, step by step](#2-boot-step-by-step) |
[3. Writing a program](#3-writing-a-program) |
[4. What is supported, by ABI version](#4-what-is-supported-by-abi-version) |
[5. Files on the card](#5-files-on-the-card) |
[6. Developing over USB](#6-developing-over-usb) |
[7. Make your own JavaScript OS](#7-make-your-own-javascript-os) |
[8. Limits and gotchas](#8-limits-and-gotchas)

## 1. What the kernel is

sandbox-kernel owns the hardware: the MMU, the timer, USB, the SD card, the e-ink status
panel, the HDMI framebuffer and the watchdog. It runs in privileged mode and never runs
your code in that mode.

Your OS is **one user-mode ARM ELF file on the SD card**. The kernel loads it into its
own pages, starts it, and answers its system calls (`svc #0`). The program can only reach
the screen, input, clock and files through those calls, so a bug in it cannot take the
kernel down. If the program faults, exits with a nonzero status, or stops sending its
heartbeat, the kernel takes the screen back, cleans up, and restarts it or falls back to a
diagnostics program (see [the fault budget](#the-fault-and-restart-budget)). The kernel
keeps its USB command console running in the default device build, so you can replace a
bad program from a computer. The separate host keyboard build has no USB recovery link
(see section 4).

The first OS written for it is Game-Maker OS (`GMOS.ELF`, in the separate
`gamemaker-os` repository). The kernel is not tied to it: any ELF that follows section 3
runs.

## 2. Boot, step by step

### GPU firmware and `config.txt`

The Pi's GPU runs first. It reads `bootcode.bin`, `start.elf` and `fixup.dat` from the
FAT32 boot partition (copied by `scripts/assemble_boot.py`), then `config.txt`, then
loads `kernel.img`. The generated `config.txt` is:

| Line | What it does |
| --- | --- |
| `kernel=kernel.img` | the file the firmware loads |
| `kernel_address=0x8000` | where it loads it |
| `arm_64bit=0` | the ARM core starts in 32-bit mode |
| `device_tree=` | empty: no device tree is passed; the kernel needs none |
| `gpu_mem=32` | gives the GPU only 32 MB, leaving the rest to the ARM side |
| `core_freq=250`, `core_freq_min=250`, `force_turbo=0` | fixed GPU core clock of 250 MHz |
| `enable_uart=1`, `init_uart_clock=3000000` | UART0 on GPIO14/15 at 115200 8N1 (this takes the UART from Bluetooth) |
| `hdmi_force_hotplug=1` | HDMI is on even if no display is seen at power-on |
| `hdmi_group=1`, `hdmi_mode=16` | 1920x1080 at 60 Hz (CEA mode 16) |
| `hdmi_drive=2` | HDMI signaling, including audio packets (`scripts/assemble_boot.py:28`) |
| `hdmi_ignore_cec=1`, `hdmi_ignore_cec_init=1` | the firmware's own CEC task stays off the bus and sends no start-up Active Source; the kernel's CEC driver is the only device (`docs/cec.md`) |
| `disable_overscan=1` | no black border |
| `scaling_kernel=8` | the GPU scales smaller modes to 1080p with nearest-neighbor, so 960x540 is exactly 2x |

### `kernel.img`, the selector

`kernel.img` is a small fixed **selector**. It reads the boot-control records, picks a
runtime image, checks its hash, and jumps to it. Runtime payloads load and enter at
`0x100000` (`docs/m0.md`, "The fixed selector").

### KERNA, KERNB and the BOOTCT records

The runtime kernel lives in one of two 2 MiB files in the card's root, `KERNA.BIN` and
`KERNB.BIN` (slots A and B). Two 4096-byte files, `BOOTCT0.BIN` and `BOOTCT1.BIN`, hold
the **control record**: which slot is confirmed, which is pending, whether a trial boot has
been used, and the SHA-256 of each (`docs/m0.md`, the control record table).

- A new kernel is written to the inactive slot over USB (`PUT2`) and marked pending.
- On the next boot the selector checks the slot's hash and records that the trial was
  used *before* entering it. If the trial dies, the next boot picks the previous
  confirmed slot.
- The default device runtime confirms itself after **30 seconds** of healthy timer, USB and storage
  progress (`core/main.c`, "Trial confirmed after 30 seconds").
- Each change writes the older of the two control files, so one valid copy always
  survives. The control files are preallocated, so the kernel never allocates clusters
  for them.

You do not touch any of this to write an OS program; it matters when you update the kernel
itself with `usb_kernel.py send`.

### The runtime starts (`kernel_main`, `core/main.c`)

In order:

1. **Input:** `input_init` creates the input device before panel, USB or SD setup.
2. **E-ink "starting" screen** (the mark, `SANDBOX KERNEL`, `STARTING...`) finishes
   before MMU, UART, interrupts and USB initialization.
3. **Card scan:** `store_init` initializes storage and verifies the fallback slot. SD
   failures are logged and shown on `ERR:`; the default device USB console stays available.
4. **HDMI and audio:** `fb_start` requests the kernel's 960x540x32 framebuffer and draws
   its pattern, then `hdmi_audio_init` tries the audio backend. Framebuffer failure is
   logged and boot continues. Only afterward does `asset_probe` list up to 12 root names
   and read `MIT.TXT` with a CRC (`core/main.c:31`, `core/main.c:40`, `core/main.c:54`).
5. **Autostart countdown:** with ready storage the log says
   `OS autostart in 10 s: <path>`. The deadline is 10 seconds from entry to `kernel_main`.
   An admitted USB `RUN` cancels autostart even if loading fails; `STOP` also cancels it.
   A busy file or kernel transfer only postpones launch. `INFO`, `KEY`, `AXIS` and `INPUT`
   do not cancel it; a completed `PUTF` can be followed immediately by autostart after the
   deadline. Use `stop` before installing if you need autostart canceled
   (`core/main.c:69`, `core/protocol.c:80`, `core/os.c:214`).
6. At or after the deadline, when the protocol is no longer busy, the kernel launches
   the chosen program (`core/main.c:70`).

### Which program autostarts (`core/autostart.c`)

`autostart_pick` chooses, in order:

1. The first line of `/OS/AUTORUN.TXT`, if it names a program that exists. The name is
   1 to 8 characters of `A-Z 0-9 _ -` followed by `.ELF`; case is folded, and blanks and
   a trailing CR are ignored. Anything else on line 1 makes it invalid. Later lines are
   free for notes. If `AUTORUN.TXT` exists but its first line is invalid or names an
   unavailable program, the choice is logged ("ignored") and skipped. A missing
   `AUTORUN.TXT` is skipped without a log (`core/autostart.c:33`).
2. `/OS/GMOS.ELF`, if present.
3. `/OS/DIAG.ELF`, the diagnostics program.

There is no user unlink call. To go back to the default order, replace `AUTORUN.TXT`
with a first line of `GMOS.ELF` (or a blank one, which is ignored).

### Rejected start: the DIAG.ELF fallback

If the loader rejects the chosen file (`os_start` returns an error; the log says
`OS reject <path> <code>`, such as `ELF QUOTA`), `autostart_run` logs
`OS autostart rejected; trying DIAG.ELF` and starts `/OS/DIAG.ELF` instead. If that is also
missing or bad, the kernel keeps its recovery console.

### The fault and restart budget

`core/os.c` (`os_policy_restart`) allows **one restart per 60 seconds**. These count as a
fault: a CPU abort or undefined instruction, no readiness report in 10 s, a missed
heartbeat (2 s), or an exit with a **nonzero** status. After a fault:

- first fault in a 60 s window: the program is cleaned up and restarted;
- a second fault inside 60 s: the kernel gives up on it. If it was the autostarted
  program, the kernel starts `/OS/DIAG.ELF` and logs
  `OS fell back to DIAG.ELF after repeated faults` (also the e-ink `ERR:` line). If it was
  DIAG itself, or a program you started with USB `RUN`, you get the recovery console.

A clean exit with status 0 is not a fault: the log says `OS exit 0 <path>; recovery
console`, and the last frame stays on the HDMI screen. After a fault the kernel puts its
own 960x540 pattern back. A USB `RUN` starts a fresh budget. A program started with `RUN`
never falls back to DIAG.

### The recovery console

In the default device build, the "recovery console" is the kernel with no OS program running: the USB CDC command
loop (`core/protocol.c`) keeps answering `INFO`, `PUTF`, `RUN`, `STOP` and the input
commands. Log lines say `OS none; recovery console (USB PUTF and RUN available)` or
`OS stopped; kernel recovery console kept`.

### The kernel's own screens

The kernel carries its own generic branding (the chip-with-K mark and the name
`sandbox-kernel`); its boot screens use that branding. A running program can replace the e-ink title,
mark, build line or page through the ABI 1.9 API below. The art is in `art/boot/` (with its layout notes), baked into
`core/logo_data.c` by `scripts/make_boot_art.py`.

- **HDMI boot screen:** color bars over a 16-step gray ramp, with a small panel in the
  middle (the mark, `sandbox-kernel`, `bare-metal boot`). It is drawn at 960x540 (the
  kernel's own mode) and 640x480, and again whenever a fault puts the kernel's screen back.
- **HDMI recovery console:** when no OS program is running (none was found at boot, or USB
  stopped it) the kernel shows a header and a 60 by 12 box with its own log lines, newest
  at the bottom, redrawn as lines arrive. A program that exits on its own leaves its last
  frame up, as before. The console is 960x540 only.
- **E-ink:** the starting and status screens below; a running OS can change the title and
  mark or show its own page (`docs/eink.md`, section "Show your OS on the e-ink").

`scripts/render_screen.py OUTDIR` renders all of them on the host to PNG.

### Reading the e-ink panel

The 250x122 panel (`core/render.c`, `render_status`) shows the chip-with-K mark, the title
`SANDBOX KERNEL`, the build ID, and three lines (values are clipped at 16 characters):

| Line | Meaning |
| --- | --- |
| `UP:` | uptime since the kernel started |
| `USB:` | USB state from `usb_state` (`drivers/usb.c`): `disconnected`, `enumerated` (the computer sees the device), `reader` (a program has the serial port open), `suspended`, `OTG (KEYBOARD NEXT)` (the OTG ID pin read grounded once at boot, latched, and no computer has enumerated for 500 ms; the kernel does not start host mode, hot-plug is not implemented yet, so replugging an adapter later changes nothing until reboot, and this device build does not support a USB keyboard), or a start-up failure such as `USB POWER MBOX TIMEOUT` |
| `ERR:` | the kernel's **last error** (`last_error`), `none` if there has been none. It is overwritten by each new error and never cleared. Normal refusals (`not-ready`) do not set it |

Some `ERR:` values you may meet: `Panel BUSY timeout`, `No verified fallback`,
`Timer progress lost`, `Trial storage health failed`, an SD step name when the card did not
start, and `OS autostart rejected; trying DIAG.ELF`. Which replies set `ERR:` (`core/protocol.c`):

| Reply | Sets `ERR:`? |
| --- | --- |
| `fail()` errors: bad command, `size`, `crc-format`, CRC or SHA-256 mismatch, write failure | yes |
| storage not ready (`PUTF`, `RUN`, `PUT2` when storage is down): `ERR not-ready` through `fail()` | yes (`not-ready`) |
| flow-control refusals through `refuse()`: no `INFO` yet, an OS program running, a locked or pending update | no, reply only |
| direct replies: `ERR input value` (`KEY`, `AXIS`), `ERR run ASSET NOT FOUND` (`RUN`) | no, reply only |

`STOP` and `INPUT` use `refuse()` for their not-ready cases. A refusal never clears an earlier error.
The panel normally redraws when its content changes, but even an unchanged static page
gets periodic full maintenance refreshes (`drivers/panel.c:64`, `core/eink.c:65`).
The maintenance deadline is 23 hours after the last full refresh (`include/eink.h:17`);
the physical refresh timing and cadence remain **hardware-untested**.

### Show your OS on the e-ink (ABI 1.9)

While your program runs it can put its own name and mark on the panel, or draw a whole
250x122 page, through `/dev/eink` (write-only; full rules and limits in `docs/eink.md`):

```c
#include <sandbox/syscall.h>
extern const uint8_t my_page[SBX_EINK_PAGE_BYTES]; /* 250x122, 32-byte rows, bit 7 = leftmost, 1 = black */

int main(const sbx_startup *startup) {
  if (startup->major != 1 || startup->minor < 9) return 1;
  uint64_t eink, sequence = 0;
  if (sbx_open("/dev/eink", 9, SBX_O_WRONLY, &eink) != 0) return 1;
  sbx_eink_title_rec t;
  int title_pending = 1, page_pending = 1;
  for (;;) {
    if (sbx_health(startup->generation, ++sequence) != 0) return 1;
    /* Service input and other bounded work here on every pass, including retries. */
    int32_t rc = 0;
    if (title_pending) {
      rc = sbx_eink_title(eink, &t, "MY OS", 5, NULL, "V0.4 3A9F2C1");
      if (rc == 0) title_pending = 0;
    } else if (page_pending) {
      rc = sbx_eink_page(eink, my_page);
      if (rc == 0) page_pending = 0;
    }
    if (rc != 0 && rc != -SBX_EAGAIN) return 1;
    uint64_t now;
    if (sbx_clock_ns(&now) != 0) return 1;
    if (sbx_sleep_until(now + 100000000ull) != 0) return 1; /* 100 ms, then service health again */
  }
}
```

TITLE and PAGE share the gate: accepted changes must be at least 1 s apart, with at
most 30 in any 60 s (`core/eink.c:17`, `core/eink.c:38`, `core/eink.c:42`). Keep a
refused update pending and retry through the health-servicing main loop, as above.
An empty busy retry or sleep alone is unsafe: the gate can stay closed longer than
the 2 s heartbeat deadline, and sleep does not service health (`core/os.c:151`,
`core/sys.c:385`). The example checks title, page, health, clock and sleep errors;
it keeps servicing health after the page is accepted. The SDK wrappers are in
`sdk/include/sandbox/syscall.h:123`, `sdk/include/sandbox/syscall.h:135` and
`sdk/include/sandbox/syscall.h:167`.

- **Limits:** one accepted update per second at most and 30 a minute; a refused one returns
  `-SBX_EAGAIN` and changes nothing. The OS cannot ask for a full refresh; the kernel does a
  ghost-clearing one itself after 50 partials, with full refreshes at least 180 seconds
  apart and maintenance due after 23 hours (`include/eink.h:12`, `core/eink.c:65`).
  These are implemented policies; the constants marked `VERIFY` are hardware-untested limits.
- **No panel:** `/dev/eink` and its calls return `-SBX_ENODEV` when no panel driver is running (or it was disabled after a fault).
- **The kernel wins:** when your program exits, faults or is stopped, the panel goes back to
  the kernel's status screen. If a kernel error appears while your page is up, the panel shows
  the kernel screen at its next update; your next page returns with a small black square in
  the top-right corner (8x8) for as long as `ERR:` is not `none`. Leave that corner white.

The device is singleton (`EBUSY` on a second open), write-only (`EACCES` for other
access), and absent/disabled panels return `ENODEV` (`core/sys.c:109`). Class 4 requests
are `EINK_QUERY=0x40000` (80-byte record), `EINK_TITLE=0x40001` (272 bytes), and
`EINK_CLEAR=0x40002` (16-byte header). QUERY reports width, height, page bytes, title
maximum, mark bytes, minimum interval in ms, maximum updates per minute and flags=0
(`core/sys.c:281`, `sdk/include/sandbox/abi.h:227`).

Write exactly 3,904 bytes: 122 rows of 32 bytes for 250 pixels, MSB leftmost, 1=black.
An optional mark is 40x40 pixels, 200 bytes. TITLE takes 1–16 name bytes and 0–16 build
bytes; unused name/build tails and absent mark bytes must be zero. Lowercase becomes
uppercase; unsupported title glyphs return `EINVAL`, unsupported build glyphs become
`?` (`include/eink.h:6`, `core/sys.c:292`, `core/eink.c:26`). A successful change accepts
content for a later panel task; it does not promise a completed physical refresh.
Closing the handle alone leaves the composed page; program cleanup restores kernel
ownership (`core/sys.c:69`, `core/sys.c:77`, `core/eink.c:51`).

## 3. Writing a program

Short on time? Copy [`examples/hello/`](../examples/hello/README.md): a complete program (screen, arrow keys, heartbeat, clean exit) in about 110 lines, with a Makefile that uses only the SDK.

The examples request 960x540 RGB565, then query the framebuffer and use the
negotiated width and height for clipping, movement bounds and presentation, and
its pitch for row addresses. A successful mode request may return a smaller mode.
Map the queried buffer size and check `sbx_fb_present` errors; both examples exit
on a failed present. CLOCK scales its digits to the returned screen; HELLO clips
its fixed text and box. Initialize syscall outputs and check return status before
using them. Scalar outputs such as fd, transferred count and mapped address are assigned
only on success. Query wrappers initialize the caller record before the ioctl, so a failed
query changes it: this applies to framebuffer, audio, input, input state, e-ink and serial
(`sdk/include/sandbox/syscall.h:47`, `sdk/include/sandbox/syscall.h:97`,
`sdk/include/sandbox/syscall.h:116`, `sdk/include/sandbox/syscall.h:186`). Both examples exit on
input-open or clock failure. For nonblocking input, `-SBX_EAGAIN` ends the current
drain; other read errors or incomplete events cause an explicit failure exit.

### The ELF rules (`core/elf.c`, `include/uspace.h`)

These are the main loader rules; the complete format checks are in `core/elf.c:21`.
The loader reports a short rejection code; it does not validate every instruction as ARMv6:

- 32-bit little-endian ELF, type `ET_EXEC` (not PIE or shared), machine ARM, EABI version
  5, **soft-float** (`ELF HARD FLOAT` otherwise). Build for **ARMv6 (ARM1176)** in ARM
  state; an entry address with bit 0 set is Thumb and is rejected (`ELF THUMB`), and the
  entry must be 4-byte aligned.
- Loadable segments must lie in `0x08000000` to `0x10000000` (`U_IMAGE_BASE`,
  `U_IMAGE_END`). The SDK link map puts text at `0x08000000` and data on separate pages.
- At most **4 nonempty loadable segments** (`ELF_MAX_SEGMENTS`), no two sharing a page.
  Zero-size `PT_LOAD` entries do not count; at least one nonempty segment is required.
- No segment is both writable and executable (`ELF WX`); an executable stack is rejected.
- No dynamic linking, interpreter, TLS, relocations or imports. Link statically.
- The entry needs four file-backed bytes inside an executable segment (`core/elf.c:73`).
- OSABI must be 0 or 97. ELF/program headers are 52/32 bytes; there must be 1–16
  program headers, in a bounded table starting at or after byte 52 and aligned to 4.
  An optional section table has 40-byte entries, at most 256, is aligned to 4 and fits
  inside the file (`include/elf.h:8`, `core/elf.c:27`, `core/elf.c:35`, `core/elf.c:77`).
- **Image quota:** the pages of all segments together (including zeroed `.bss`) may not
  exceed `U_IMAGE_QUOTA`, **8 MiB**. Larger is `ELF QUOTA`.
- The file itself may be at most 1 MiB for a USB install (`FATPUT_MAX`) and for loading.
- **Stack:** 1 MiB (`U_STACK_BYTES`), top at `0x7fff0000`, with a guard MiB below it.
- **Handles:** 16 total handles per program (`include/sys.h:9`); a closed or stale id is `EBADF`.
  The SD backend has only eight handles shared by regular files and directories; the
  ninth SD open can return `EMFILE` (`include/assets.h:6`, `core/assets.c:132`).
- The framebuffer maps at `0x40000000`. A program also gets a fixed 16 MiB pool of user
  pages in all, shared by image, stack, startup page, framebuffer and the optional app arena.

### The startup record and the ABI check

At entry, `r0` points to a read-only `sbx_startup` record (`sdk/include/sandbox/abi.h`):
`size`, `major`, `minor`, the boot `generation`, `trial` (1 while the kernel is on trial),
and the quotas `image_bytes`, `stack_bytes` and `handles`. `sp` is the stack top and `lr`
is the unmapped sentinel `0x0000f000`. A return causes a prefetch abort that the kernel
recognizes as exit with `r0` as status; it is not a readable trampoline page
(`include/uspace.h:18`, `core/os.c:115`, `core/os.c:176`, `sdk/lib/crt0.S:8`).

**Check `major` and `minor` before using a call that is newer than 1.0.** An older kernel
answers `-SBX_ENOSYS` (-38) for an unknown syscall number (`core/sys.c:398`). An
unknown device ioctl returns `-SBX_ENOTTY` (-25), while opening an absent device
returns `-SBX_ENODEV` (-19) (`core/sys.c:270`, `core/sys.c:304`, `core/sys.c:139`).
For example, the ABI 1.7 kernel at `98c5200` returns `ENODEV` for `/dev/audio`,
`/dev/eink` and `/dev/serial0`, which it does not have (`98c5200:core/sys.c:114`;
version at `98c5200:include/abi.h:8`). `sdk/lib/crt0.S` calls
`int main(const sbx_startup *)` and uses the return value as the exit status. A short
record is valid; from 1.6 the record grows to 48 bytes with `app_generation`, and
`sbx_app_generation()` checks size and version for you.

### Calling without the SDK

For `svc #0`, put the call number in `r7`, the record address in `r0`, its byte size in
`r1`, major version 1 in `r2`, and zero in `r3`–`r6`; status comes back in `r0`
(`sdk/include/sandbox/syscall.h:17`). Records are little-endian and 8-byte aligned,
16–4096 bytes in multiples of 8, and at least the known size for that call. The first
16 bytes are four u32 words: size (equal to `r1`), flags, reserved0, reserved1. Zero the
last three, all output fields, and every unknown trailing byte. Address slots are u64
with zero upper halves on ARM32. Output records must be writable; pointed-to buffers
need the access required by the operation. Nested ioctl records require their exact
known size and the same header/alignment rules (`core/abi.c:6`, `core/sys.c:223`,
`core/sys.c:394`, `sdk/include/sandbox/abi.h:3`). Zero outputs again before reusing a
raw record. A failed operation is not a promise that buffers or device state are untouched.

`SBX_CALL_QUERY=0` is declared but unimplemented. Calls 0, 7, 8, 10, 15 and numbers
above 21 return `ENOSYS` before record validation (`core/sys.c:394`).

### The heartbeat

Call `sbx_health` (call 17) to say you are alive. The first call is the **readiness
report** and must come **within 10 seconds** of the start; after that, one at least every
**2 seconds** (`OS_READY_US`, `OS_HEARTBEAT_US` in `include/os.h`). Only health calls
count. Miss either deadline and the program is stopped as a fault. The record carries the
startup `generation` and a `sequence` that strictly increases from 1. `sbx_sleep_until`
does not replace the heartbeat: wake up and send one.

### Exit codes

Return from `main` or call `sbx_exit(status)`. **0** is a clean exit and you are left at
the recovery console. **Any other value counts as a fault** (and uses the restart budget).

## 4. What is supported, by ABI version

The major version is 1 and the current minor is the `SBX_ABI_MINOR` in
`sdk/include/sandbox/abi.h:13` (currently **1.10**, also `include/abi.h:8`). Minors only add.
Every returning syscall returns 0 or a negative errno. `exit` does not return to user
code; `SYS_EXITED=1` is internal to the supervisor (`core/sys.c:475`, `core/os.c:171`).
Existing call numbers and record layouts stay compatible. (The first row lists the ABI 1.0 draft
described in `docs/design.md`.)

| ABI | Added |
| --- | --- |
| 1.0 | Calls `open`, `read`, `write`, `close`, `ioctl`, `mmap` (only the framebuffer), `exit`, `getdents` (call 11), `health`. Read-only files under `/sd` (the card's root). `/dev/log` (each write is a log line of up to 120 characters, shown as `OS: ...` on USB). `/dev/fb0`: the surface starts as **640x480x32** XRGB8888, `mmap` it, draw, then `FB_PRESENT` a rectangle (ioctl class 1). `/dev/input/event0`: 16-byte Linux-style events, opened read-only with `SBX_O_NONBLOCK`; `INPUT_QUERY` and `INPUT_STATE` (class 3). The default device build feeds synthetic input over USB (`KEY`, `AXIS`, `INPUT`); the separate host build supports a boot keyboard. See the input rules below. |
| 1.1 | `clock_gettime` (call 13, `sbx_clock_ns`: monotonic nanoseconds, the same clock as input events) and `sleep_until` (call 14). |
| 1.2 | `/dev/fb0` modes: `FB_SET_MODE` (width 16–1920 and a multiple of 4, height 16–1200, total bytes at most 1,228,800, in XRGB8888, RGB565 or 8-bit indexed; see the framebuffer rules below) and `FB_PALETTE` for 8-bit. **960x540x16** fits and is exactly half of 1080p. 1920x1080 does not fit. |
| 1.3 | `seek` (call 9): absolute offsets 0 through end of a regular `/sd` file. |
| 1.4 | `getdents_meta` (call 18: size, FAT date/time words, first cluster). Bounded **cache** writes under `/sd/OS/CACHE/`: `fsync` (call 12) and copy-publish `rename` (call 19); see `docs/cache-storage.md`. |
| 1.5 | `getdents_long_meta` (call 20): validated VFAT long names as UTF-8; use a buffer of at least 280 bytes. Calls 11 and 18 keep returning 8.3 aliases. |
| 1.6 | Opt-in **app arena** (call 21): one fixed block of 2,195,456 bytes at `0x30000000`, read/write, never executable, granted once per run. See `docs/app-arena-abi.md`. |
| 1.7 | Save folders: write scope `/sd/OS/SAVES/<GAME>/<NAME>` (`EDQUOT` -122 past 64 game folders or 32 files per game); see `docs/saves-storage.md`. |
| 1.8 | **Audio:** `/dev/audio`, write-only, one writer (`EBUSY` for a second open): stereo signed 16-bit little-endian at a fixed 48,000 frames a second, whole 4-byte frames only (`EINVAL` otherwise). `write` never blocks: it queues what fits in the 16,384-frame ring (about 341 ms; `include/audio_ring.h:20`) and returns that many bytes, or `EAGAIN` when nothing fits. `ioctl` `SBX_IOC_AUDIO_QUERY` (0x20000) reports rate, capacity, queued, played, underruns and overruns. Short drains are filled with silence. HDMI drains the ring when its backend starts; otherwise the null backend silently drains it. No ABI minor was added for this backend or its internal mixer. See the audio rules below and `docs/audio-device.md`. |
| 1.9 | **E-ink API:** `/dev/eink` (write-only): set the OS's name and 40x40 mark on the status screen, or `write` a whole 250x122 page; ioctl class 4; the kernel enforces refresh limits (`EAGAIN`) and takes the panel back on any fault or exit. See `docs/eink.md`. |
| 1.10 | `/dev/serial0`: a nonblocking byte stream between the OS and the host on the USB link, with a counters query (class 6). See `docs/serial.md`. |

**Sound:** `/dev/audio` is drained by an HDMI backend (the 1080p60 mode at 48 kHz, 2-channel PCM; `docs/audio-device.md`). It needs HDMI signaling, so `config.txt` carries `hdmi_drive=2`; if the backend cannot start, the kernel logs why and a silent null backend drains the ring instead. **Hardware-untested:** HDMI audio output on physical sinks and the new e-ink/serial
paths are not established by these code checks. **Not yet:** gamepads, Wi-Fi and Bluetooth (no wireless driver runs), HDMI-CEC, and the
planned `/state` journal store. Anonymous `mmap`, free or resize of the arena, and
general SD writing are not supported either.

### Framebuffer rules

`FB_QUERY=0x10000` returns the negotiated mode in a 56-byte record. Its pitch is packed
user width times bytes per pixel, not GPU pitch. `buffer_bytes` is the full 1,228,800-byte
surface at 32 bpp and the packed visible size at 16/8 bpp (`core/sys.c:230`). Formats
1/2/3 mean XRGB8888/RGB565/INDEX8 (`sdk/include/sandbox/abi.h:68`).

`FB_SET_MODE=0x10002` requires width 16–1920 divisible by 4, height 16–1200 and
width × height × bytes per pixel ≤1,228,800. Fitting alone is insufficient. Firmware may
negotiate another mode or return `EIO`; query after success (`core/sys.c:259`,
`drivers/fb_hw.c:36`, `core/fb.c:23`). `mmap` requires offset=0, flags=SHARED (1),
protection=READ|WRITE (3), length 1–1,228,800; even a smaller requested length maps the
whole surface at `0x40000000`. Closing the fd leaves it mapped until program end
(`core/sys.c:370`, `core/sys.c:433`). `FB_PRESENT=0x10001` requires that mapping and a
rectangle wholly inside the negotiated width/height. `FB_PALETTE=0x10003` has 256 RGB
slots: first=0–255, count=1–(256−first) (`core/sys.c:239`, `core/sys.c:265`).

### Input rules and USB roles

One reader owns `/dev/input/event0`; a second open returns `EBUSY`. Reads require at
least 16 bytes, floor nonmultiples to whole events, and return at most 16 events (256
bytes) per call (`core/sys.c:120`, `core/sys.c:179`, `core/input.c:54`). The queue holds
64 records, with 768 key slots (injected codes 1–767), 32 axes (codes 0–31, values
−32768–32767), and key values 0=up, 1=down, 2=repeat. Frames end with `SYN_REPORT`.
Overflow empties the queue and emits `SYN_DROPPED`: call `INPUT_STATE=0x30001` to
recover current keys/axes and empty the queue, then resume reading. Removal invalidates
existing instances with `ENODEV`; close and reopen after re-add (`include/input.h:6`,
`core/input.c:25`, `core/input.c:49`, `core/input.c:67`). `INPUT_QUERY=0x30000`
still reports SYNTHETIC even in the host build (`core/sys.c:316`).

The default `USB_ROLE=device` build supplies `KEY`/`AXIS` input and the CDC recovery
console. `make USB_ROLE=host` instead links TinyUSB HID host and supports one boot
keyboard through the same queue, with no CDC protocol or log console
(`Makefile:12`, `drivers/usb_host.c:1`, `core/usbkbd.c:36`). This hardware-untested host
path runs only as a trial and deliberately never confirms; power cycling returns to
the confirmed device kernel. Factory image assembly refuses host builds
(`core/main.c:23`, `core/main.c:85`, `scripts/assemble_boot.py:8`).

### Arena and clock rules

Arena `requested_bytes` is a minimum in 1–2,195,456; every successful first grant charges
the full block, and a repeat valid request returns the existing lease. Use startup
`app_generation`, not boot generation. Zero/stale epoch returns `EBADF`, invalid size
`EINVAL`, admission state `EBUSY`, and pool/table pressure `ENOMEM`. Admission reserves
a 3 MiB floor plus framebuffer capacity, so even a small request may fail
(`sdk/include/sandbox/abi.h:182`, `sdk/include/sandbox/syscall.h:37`,
`include/app_arena_core.h:9`, `core/app_arena_core.c:65`, `core/app_arena_core.c:88`).

Clock selector must be 0; returned nanoseconds have microsecond resolution.
`sleep_until` uses an absolute deadline, rounds up to microseconds, and returns
immediately for a past deadline (`core/sys.c:379`).

### Audio rules and diagnostics

`AUDIO_QUERY=0x20000` uses an 80-byte record: flags=0, rate, capacity_frames,
queued_frames, played_frames, underruns, overruns and overrun_frames. Counters reset
on open; close discards queued audio. A zero-byte write succeeds. Underruns count short
drain calls after the first nonempty write, rather than elapsed gaps or missing frames
(`core/sys.c:187`, `core/sys.c:306`, `core/audio_null.c:13`, `include/audio_ring.h:9`).

The HDMI backend targets 1080p60 with 48 kHz stereo PCM. Its eight 384-frame DMA
segments (8 ms each) are internal, not an application write quantum
(`include/hdmi_audio.h:69`, `include/hdmi_audio.h:104`). Startup failure logs a reason
and leaves the null drain active (`core/main.c:54`, `core/audio_null.c:44`,
`drivers/hdmi_audio.c:137`). Physical audio output remains hardware-untested here.

The internal mixer synthesizes diagnostic tones and pumps them into the same audio
ring; it does not provide another app device, ioctl, volume or tune API. It can compete
with app PCM for ring space (`core/mixer.c:19`, `core/mixer.c:85`,
`drivers/hdmi_audio.c:149`). USB `AUDIO` reports backend, drain rate, starvation, DMA,
ring, tone and mixer status. `TONE HZ MS` accepts 50–4000 Hz and 1–1000 ms, replying
`OK tone` or `ERR tone args`, `no-hdmi`, or `busy`. Both require an INFO session
(`core/protocol.c:118`, `core/audio_null.c:48`, `drivers/hdmi_audio.c:176`). HDMI-CEC
helpers are not a live app or hardware service.

### Numeric errors

The SDK declares these positive constants; syscall errors return their negatives
(`sdk/include/sandbox/abi.h:25`). A declaration alone does not promise a reachable error.

| SDK name (prefix `SBX_`) | Number | Meaning |
| --- | --- | --- |
| EPERM | 1 | operation not permitted |
| ENOENT | 2 | missing path |
| EIO | 5 | I/O failure |
| EBADF | 9 | invalid handle or arena epoch |
| EAGAIN | 11 | retry later |
| ENOMEM | 12 | allocation or mapping resources unavailable |
| EACCES | 13 | access denied |
| EFAULT | 14 | invalid pointer, alignment or memory access |
| EBUSY | 16 | resource owned or admission blocked |
| EEXIST | 17 | already exists |
| ENODEV | 19 | absent device or stale input instance |
| ENOTDIR | 20 | expected a directory |
| EINVAL | 22 | invalid argument or record |
| EMFILE | 24 | handle limit |
| ENOTTY | 25 | unsupported ioctl for this device |
| ENOSPC | 28 | no storage or staging space |
| ESPIPE | 29 | object cannot seek |
| EROFS | 30 | read-only path |
| EPIPE | 32 | broken pipe |
| ENAMETOOLONG | 36 | path length or depth limit |
| ENOSYS | 38 | unimplemented call |
| EOVERFLOW | 75 | overflow |
| EPROTONOSUPPORT | 93 | incompatible ABI major |
| ETIMEDOUT | 110 | timeout |
| EDQUOT | 122 | save folder/file quota |

EPERM, EEXIST, EPIPE and ETIMEDOUT are declarations without current app syscall return
paths; epoch EOVERFLOW is a supervisor launch failure. The storage backend can also
return **−21 (EISDIR)** from fsync for an unacceptable target and **−27 (EFBIG)** from
rename for a source larger than its 1 MiB scratch buffer; neither has an SDK constant
(`core/fatput.c:241`, `core/fatput.c:335`, `core/fatput.c:360`). USB textual errors and
loader rejection strings are separate from numeric app errno (`core/protocol.c:81`).

## 5. Files on the card

The card is an MBR disk with one FAT32 partition. The kernel mounts it as `/sd`; paths in
calls start with `/sd/`.

| Path | What it holds |
| --- | --- |
| root | firmware files, `config.txt`, `kernel.img`, `KERNA.BIN`, `KERNB.BIN`, `BOOTCT0.BIN`, `BOOTCT1.BIN`, license texts. Leave these alone |
| `/OS/*.ELF` | OS programs: `GMOS.ELF`, `DIAG.ELF`, `JS.ELF`, yours |
| `/OS/AUTORUN.TXT` | which program autostarts (section 2) |
| `/OS/APPS/<DIR>/` | an app folder for a program such as `JS.ELF` |
| `/GAMES/<DIR>/`, `/GAMES/<NAME>.GMP` | games, loose or packed (`LIBRARY.JSL` is the library index) |
| `/POCKET/<NAME>.GMP` | packed games for the Pocket Player |
| `/OS/CACHE/` | the app's disposable cache |
| `/OS/SAVES/<GAME>/` | saves and high scores |

**8.3 names.** The kernel's own writer (USB `PUTF`) accepts only upper-case 8.3 names:
1 to 8 of `A-Z 0-9 _ -`, an optional extension of up to 3. Directory enumeration returns long names
only through call 20 (ABI 1.5); calls 11 and 18 return the 8.3 alias. Read-only `open`
accepts validated VFAT UTF-8 long names with case folding, as well as 8.3 aliases; names
longer than 255 UTF-8 bytes fall back to the alias (`include/vfat.h:23`, `core/assets.c:117`). A full path,
including `/sd`, must fit in 255 bytes. Paths below `/sd` have at most 16 components;
read-only assets can be up to 64 MiB, while ELF loads and USB file writes remain limited
to 1 MiB (`include/assets.h:6`, `core/assets.c:144`, `core/os.c:98`).

**What a program may write.** Only `/sd/OS/CACHE/<NAME>` and `/sd/OS/SAVES/<GAME>/<NAME>`:
upper-case 8.3 names, no `..`, no extra directory levels. Open with
`SBX_O_WRONLY|SBX_O_CREAT|SBX_O_TRUNC`, write up to 1 MiB, then `sbx_fsync` to commit; close
**discards unpublished changes** and keeps previously committed data. One writer at a time. Rename inside the folder is
copy-then-publish, so a save written as `NAME.TMP` and renamed survives power loss as the
old file or the new one. Everything else on the card is read-only (`EROFS`); this is not
a journal, and a power cut can leave an orphan `.TMP` file.

Directory entries have an 8-byte legacy prefix or a 24-byte metadata prefix, followed
by `name_bytes` without NUL and zero padding to a multiple of 8. Types are 1=file,
2=directory. EOF is transferred=0; a buffer too small for the next entry returns
`EINVAL` and retains that entry. Metadata FAT date/time words are opaque fingerprints;
identity is first cluster, not a content hash (`sdk/include/sandbox/abi.h:196`,
`core/sys.c:338`).

Quota checks generally happen at fsync, not staging open. Repeated fsync is allowed;
later writes append to the staging buffer and require another fsync. Close the writer
before rename; identical source/destination returns `EINVAL`. Any read handle under
CACHE or SAVES, including a directory, blocks mutation across both trees. There are
no user unlink or mkdir calls (`core/sys.c:126`, `core/sys.c:205`, `core/sys.c:453`,
`core/assets.c:166`, `core/assets.c:261`, `core/fatput.c:229`, `core/fatput.c:328`).

## 6. Developing over USB

Plug the Pi's USB data port into your computer. It appears as a serial device
(`/dev/cu.usbmodem*` on macOS). `scripts/usb_kernel.py <port> <command>` (Python 3
standard library only) talks to it. Each command runs an `INFO` handshake first.

| Command | What it does |
| --- | --- |
| `info` | print the device's `INFO` line: slots, generations, `updates=ready\|locked\|busy`, `os=running\|stopped\|none`, features |
| `tail` | print the kernel log (`LOG ...` lines) until you stop it |
| `put FILE [--name NAME]` | install FILE as `/OS/<NAME>` (upper-case 8.3, 1 byte to 1 MiB, CRC and SHA-256 checked) |
| `putgame FOLDER DIR` | install every file as `/GAMES/<DIR>/<NAME>` |
| `putapp FOLDER DIR` | install every file as `/OS/APPS/<DIR>/<NAME>` |
| `run NAME [--seconds N]` | start `/OS/<NAME>` now and show the log; starts a fresh fault budget and cancels autostart |
| `stop` | stop the OS program at a safe boundary; the recovery console stays |
| `key NAME...` | press keys on the synthetic input device: `up down left right enter esc q space`, a Linux code, or `NAME:down` / `NAME:up` / `NAME:repeat` |
| `axis CODE VALUE` | set an axis on the synthetic device |
| `input add\|remove` | plug or unplug the synthetic device |
| `console` | two-way OS serial session; Ctrl-] exits |
| `audio` | USB `AUDIO` backend and mixer diagnostics |
| `tone HZ MS` | USB `TONE` diagnostic tone; see section 4 |
| `send KERNEL.SKI` | update the kernel itself (A/B slot, trial boot) |

**The `INFO` fields:** `updates=ready` means this kernel accepts update requests (a capability);
`updates=locked` means it does not; `updates=busy` means a new kernel is already pending, so wait
for its confirmation. `os=running` is an OS program running now, `os=stopped` one ran and was
stopped, `os=none` none has run since boot. `features=` is **legacy**, kept for compatibility:
it still reads `kernel,no-os` when updates are ready (it never meant "no OS") and `locked`
otherwise, so older `usb_kernel.py` clients keep working. New clients read `updates=` and `os=`.

`INFO 2` is protocol version 2. Its literal `ABI=1.0` is stale and does **not** identify
the app ABI: startup reports the current major/minor (1.10 on this branch). Gate app
features on startup, never that INFO field (`core/protocol.c:135`, `core/os.c:111`,
`include/abi.h:8`). INFO also reports `profile=zero-w-armv6`, `capacity=2097152`,
`install_timeout=120`, and `serial=usb|none` (`core/protocol.c:143`).

`run --seconds` defaults to 15; `key --gap` defaults to 0.3 seconds and `--seconds` to 2.
The command table and options are implemented in `scripts/usb_kernel.py:228`.
`put --name GAMES/NAME.GMP` or `POCKET/NAME.GMP` installs packed games, with `.G01`–`.G99`
continuations; `GAMES/LIBRARY.JSL`, `GAMES/DIR/NAME` and `APPS/DIR/NAME` are also accepted.
Folder installs are shallow, sorted, skip dotfiles and uppercase filenames
(`scripts/usb_kernel.py:103`, `scripts/usb_kernel.py:123`).

**When `PUT2` is admitted:** `INFO` was sent this session, storage is ready, `updates=ready`
(not locked) and no update slot is pending. Otherwise `ERR not-ready`. `os=running` alone does
not block a kernel update. `usb_kernel.py send` parses `INFO` into exact fields and refuses
before sending if `updates=`, `features=` or `pending=` repeats, `updates=` is not exactly `ready`
(including `busy`), `features=locked` contradicts it, or a `pending=` field is not a plain
decimal 0. With no `updates=` field it accepts
only the exact legacy `features=kernel,no-os`. The web page (`web/src/piRemote/protocol.ts`, in
the GameMaker repo) reads `INFO` as a generic map and never authorizes updates.

**The not-ready rule:** `PUTF` (and so `put`, `putgame`, `putapp`) is refused with
`ERR not-ready` while an OS program is running, and during a kernel update. **Run `stop`
first**, then install, then `run`. If you are fast, you can also install during the first
10 s after boot, before autostart. Install a new `JS.ELF` or `GMOS.ELF` the same way.

### Transfer framing and errors

Commands are at most 255 printable ASCII bytes plus newline. PUTF and PUT2 reply
`READY 1024`; send blocks of up to 1024 bytes and wait for cumulative `ACK <bytes>`
after each nonfinal block. The final block gets `VERIFY`, then `OK file ...` or
`OK pending rebooting`. Receive inactivity times out after 15 seconds; PUT2 installation
after 120 seconds. PUT2 containers are 64–2,097,152 bytes (`core/protocol.c:67`,
`core/protocol.c:148`, `core/protocol.c:165`, `core/protocol.c:183`, `core/protocol.c:215`).
After VERIFY, a lost final kernel-update response means reconnect and query INFO to
resolve the pending generation; never blindly resend (`scripts/usb_kernel.py:95`).

Textual errors include `command`, `command-too-long`, `size`, `crc-format`, `crc`,
`sha256`, `not-ready`, `timeout`, `trailing-input`, `manifest`, `install-timeout`,
`write`, `readback`, `padding`, `generation` and `commit-uncertain-query-info`.
PUTF can forward storage-specific `fatput_fail` text. Input replies include
`ERR input command`, `value`, `no-device` and `range`; stop failures use `ERR stop <os_fail>`.
Check replies and resolve uncertain update commits with INFO (`core/protocol.c:64`,
`core/protocol.c:87`, `core/protocol.c:145`, `core/protocol.c:158`, `core/protocol.c:165`,
`core/protocol.c:219`).

### Talking to your OS over serial

Your program can send text to the host and receive bytes from it over the same USB link that
carries the synthetic keyboard, with `/dev/serial0` (ABI 1.10). Open it nonblocking, `write` lines,
and `read` until you get `EAGAIN`. Output with no listener, or past the 512-byte buffer, is dropped
and counted, never blocks you. On the Mac, `usb_kernel.py <port> console` shows your output as it
arrives (kernel `LOG` lines dimmed) and sends what you type, with arrows, Enter, Space, Q and Esc
also pressed as keys. Wire format, counters and limits are in `docs/serial.md`. The core is
`uint64_t fd; sbx_open("/dev/serial0", 12, 2 | 0x100, &fd);` (read and write, nonblocking), then a write call with `"hello\n"` and its length.

Serial has one owner (`EBUSY` otherwise) and requires NONBLOCK. Access modes read,
write and read/write (0/1/2) are accepted, but the current implementation does not
retain them to enforce direction on later I/O (`core/sys.c:103`, `core/sys.c:161`).
Reads return at most 256 bytes, with `EAGAIN` when empty. Writes report the full count
even for dropped bytes: inspect `SERIAL_QUERY=0x60000`, a 72-byte record containing
transport (0=none, 1=USB), TX/RX accepted, dropped and pending counts. Close clears both
queues; disconnect drops queued TX; counters persist across opens (`include/serial.h:9`,
`core/serial.c:7`, `core/serial.c:47`, `core/sys.c:272`).

Wire input is `TX <1–64>\n` followed by that many raw bytes, requiring INFO and a
2-second body inactivity limit (`ERR tx length`, `ERR tx-timeout`, or `OK tx <accepted>`).
OS output uses `OS <escaped text>` for newline-terminated chunks and `OS~ <escaped text>`
for continuations, up to 64 data bytes each. Backslashes become `\\`; controls and
non-ASCII bytes become `\xHH`; the line buffer is 272 bytes. These are separate from
kernel `LOG` lines and `/dev/log` output (`core/protocol.c:105`, `core/protocol.c:206`,
`core/serial.c:28`). `console` uses terminal cbreak mode, decodes these lines, and sends
printable text/Enter as TX plus the supported synthetic keys; `tail` prints incoming
wire lines without decoding OS escapes or sending typed input
(`scripts/usb_kernel.py:148`, `scripts/usb_kernel.py:177`).

## 7. Make your own JavaScript OS

(For a native C starting point instead, see [`examples/hello/`](../examples/hello/README.md).)

The shortest path needs no C. The `gamemaker-os` repository builds `JS.ELF`, a native
program that runs a web-page-like app written in JavaScript (QuickJS-ng). Its API (DOM
subset, a CSS subset, a 2D canvas, `game`, `fs`, `screen.setMode`) is in
[`docs/js.md`](https://github.com/lewismoten/gamemaker-os/blob/develop/docs/js.md); the
measurements and reasoning are in [`js-spike.md`](js-spike.md).

1. Build `JS.ELF` (`make js` in `gamemaker-os`) and install it: `usb_kernel.py <port> put
   build/JS.ELF`. It must fit the 8 MiB image quota (about 790 KB stripped).
2. Write `MAIN.JS` (and optionally `INDEX.HTM`) in a folder, and install it with
   `usb_kernel.py <port> putapp <folder> <DIR>`; it lands in `/OS/APPS/<DIR>/`.
3. Write `/OS/AUTORUN.TXT` with `JS.ELF` on line 1 and the app (such as `APPS/<DIR>`) on
   line 2, per `docs/js.md`. The kernel reads only line 1; JS.ELF reads line 2.
4. Reboot, or `usb_kernel.py <port> run JS.ELF`.

From JavaScript, files are read-only (`fs.readFile`, `fs.readdir`; writing files is listed
as left out in `docs/js.md`).

## 8. Limits and gotchas

- **The 8 MiB image quota is real.** GMOS hit it on 2026-11-09 and was rejected with
  `ELF QUOTA` until its picture buffers were reused (gamemaker-os `make quota`
  guards it). Large static arrays count even when they are `.bss`, because whole pages
  are charged.
- **Heartbeat:** the first `sbx_health` within 10 s, then every 2 s. A long load or sleep
  without one is a fault. Load in pieces and heartbeat between them.
- **Nonzero exit is a fault.** Exit 0 only when you mean to stop cleanly.
- **Two faults in 60 s ends your program.** An autostarted one is replaced by `DIAG.ELF`;
  a `RUN` one is not.
- **Read-only card.** Only the cache and save folders can be written, and only after
  `fsync`. A writer open blocks other mutation (`EBUSY`); a read handle on those folders
  blocks writing too, so close directory handles first.
- **Handles:** 16 total, at most eight SD files/directories, one framebuffer handle,
  one input reader, one audio writer, one e-ink handle and one serial owner
  (`include/assets.h:6`, `core/sys.c:94`, `core/input.c:54`, `core/serial.c:7`).
- **Check your minor version.** Gate features on the startup ABI version: with major 1,
  require minor >= 7 for saves, >= 8 for audio, >= 9 for e-ink and >= 10 for serial
  (`include/abi.h:8`). Fall back to silence or no saves when the version is lower.
  Unsupported operations do not all return `ENOSYS`: unknown syscall numbers return
  `ENOSYS` (-38), unknown device ioctls return `ENOTTY` (-25), and absent device
  paths return `ENODEV` (-19) (`core/sys.c:398`, `core/sys.c:270`, `core/sys.c:304`,
  `core/sys.c:139`). The ABI 1.7 ancestor also returns `ENODEV` for the then-absent
  audio, e-ink and serial devices (`98c5200:core/sys.c:114`).
- **Framebuffer size:** a program starts in 640x480x32. A mode that does not fit the
  surface or violates the width, height or format bounds is refused (`core/sys.c:262`).
- **Transfers:** at most 1 MiB per read or write call.
- **Documentation drift:** `tests/test_docs_abi.py` fails when `SBX_ABI_MINOR` in
  `abi.h` is newer than the last row of the table in section 4. Add the row when you add
  a minor.

Further reading: `docs/design.md` (the original design; parts are still plans),
`docs/m1.md` (what was built, in order), `docs/syscall-acceptance.md`, `sdk/README.md`,
and `programs/diag` for a small example program.
