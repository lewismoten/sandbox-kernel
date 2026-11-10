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
keeps its USB command console running the whole time, so you can always replace a bad
program from a computer.

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
- The runtime confirms itself after **30 seconds** of healthy timer, USB and storage
  progress (`core/main.c`, "Trial confirmed after 30 seconds").
- Each change writes the older of the two control files, so one valid copy always
  survives. The control files are preallocated, so the kernel never allocates clusters
  for them.

You do not touch any of this to write an OS program; it matters when you update the kernel
itself with `usb_kernel.py send`.

### The runtime starts (`kernel_main`, `core/main.c`)

In order:

1. **E-ink "starting" screen** (the mark, `SANDBOX KERNEL`, `STARTING...`), drawn before USB or
   the SD card is set up.
2. MMU, UART, interrupts and USB are initialized. The log prints the build ID, the reset
   cause, the selected slot and the generation.
3. **Card scan:** the SD card is initialized (`store_init`), the fallback slot is
   verified, then `asset_probe` (`core/probe.c`) mounts FAT32, lists up to 12 root
   entries, reads `MIT.TXT` with a CRC, and logs the results as `BOOT ASSET ...` lines.
   An SD failure is logged with a step name and shown on the e-ink `ERR:` line; the
   kernel still runs and the USB console still works.
4. **Input:** the synthetic input device is created (`input_init`); see section 4.
5. **HDMI:** `fb_start` (`core/fb.c`) asks the firmware for a framebuffer and draws the
   kernel's own 960x540x32 test pattern. If the firmware refuses, the log says so and
   boot continues with no picture.
6. **Autostart countdown.** If the card is ready, the log says
   `OS autostart in 10 s: <path>`. The 10 seconds are counted from the start of
   `kernel_main`. A USB `RUN` (or any USB command still in progress) during this time
   takes over, and cancels the autostart. This is the window for replacing a bad
   program over USB before it runs.
7. At 10 s the kernel starts the chosen program.

### Which program autostarts (`core/autostart.c`)

`autostart_pick` chooses, in order:

1. The first line of `/OS/AUTORUN.TXT`, if it names a program that exists. The name is
   1 to 8 characters of `A-Z 0-9 _ -` followed by `.ELF`; case is folded, and blanks and
   a trailing CR are ignored. Anything else on line 1 makes it invalid. Later lines are
   free for notes. An invalid or missing choice is logged ("ignored") and skipped.
2. `/OS/GMOS.ELF`, if present.
3. `/OS/DIAG.ELF`, the diagnostics program.

The kernel cannot delete files. To go back to the default order, replace `AUTORUN.TXT`
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

The "recovery console" is just the kernel with no OS program running: the USB CDC command
loop (`core/protocol.c`) keeps answering `INFO`, `PUTF`, `RUN`, `STOP` and the input
commands. Log lines say `OS none; recovery console (USB PUTF and RUN available)` or
`OS stopped; kernel recovery console kept`.

### The kernel's own screens

The kernel carries its own generic branding (the chip-with-K mark and the name
`sandbox-kernel`); it never shows the name of any OS program. Anyone running an OS on it
sees only that. The art is in `art/boot/` (with its layout notes), baked into
`core/logo_data.c` by `scripts/make_boot_art.py`.

- **HDMI boot screen:** color bars over a 16-step gray ramp, with a small panel in the
  middle (the mark, `sandbox-kernel`, `bare-metal boot`). It is drawn at 960x540 (the
  kernel's own mode) and 640x480, and again whenever a fault puts the kernel's screen back.
- **HDMI recovery console:** when no OS program is running (none was found at boot, or USB
  stopped it) the kernel shows a header and a 60 by 12 box with its own log lines, newest
  at the bottom, redrawn as lines arrive. A program that exits on its own leaves its last
  frame up, as before. The console is 960x540 only.
- **E-ink:** the starting and status screens below.

`scripts/render_screen.py OUTDIR` renders all of them on the host to PNG.

### Reading the e-ink panel

The 250x122 panel (`core/render.c`, `render_status`) shows the chip-with-K mark, the title
`SANDBOX KERNEL`, the build ID, and three lines (values are clipped at 16 characters):

| Line | Meaning |
| --- | --- |
| `UP:` | uptime since the kernel started |
| `USB:` | USB state from `usb_state` (`drivers/usb.c`): `disconnected`, `enumerated` (the computer sees the device), `reader` (a program has the serial port open), `suspended`, `OTG (KEYBOARD NEXT)` (the OTG ID pin read grounded once at boot, latched, and no computer has enumerated for 500 ms; the kernel does not start host mode, hot-plug is not implemented yet, so replugging an adapter later changes nothing until reboot, and the USB keyboard is **not yet** supported), or a start-up failure such as `USB POWER MBOX TIMEOUT` |
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
The panel redraws only when its content changes, and a full refresh takes seconds.

## 3. Writing a program

### The ELF rules (`core/elf.c`, `include/uspace.h`)

The loader rejects anything else, with a short code in the log:

- 32-bit little-endian ELF, type `ET_EXEC` (not PIE or shared), machine ARM, EABI version
  5, **soft-float** (`ELF HARD FLOAT` otherwise). Build for **ARMv6 (ARM1176)** in ARM
  state; an entry address with bit 0 set is Thumb and is rejected (`ELF THUMB`), and the
  entry must be 4-byte aligned.
- Loadable segments must lie in `0x08000000` to `0x10000000` (`U_IMAGE_BASE`,
  `U_IMAGE_END`). The SDK link map puts text at `0x08000000` and data on separate pages.
- At most **4 loadable segments** (`ELF_MAX_SEGMENTS`), no two sharing a page.
- No segment is both writable and executable (`ELF WX`); an executable stack is rejected.
- No dynamic linking, interpreter, TLS, relocations or imports. Link statically.
- The entry must fall inside an executable segment.
- **Image quota:** the pages of all segments together (including zeroed `.bss`) may not
  exceed `U_IMAGE_QUOTA`, **8 MiB**. Larger is `ELF QUOTA`.
- The file itself may be at most 1 MiB for a USB install (`FATPUT_MAX`) and for loading.
- **Stack:** 1 MiB (`U_STACK_BYTES`), top at `0x7fff0000`, with a guard MiB below it.
- **Handles:** 16 open files per program (`SYS_HANDLES`); a closed or stale id is `EBADF`.
- The framebuffer maps at `0x40000000`. A program also gets a fixed 16 MiB pool of user
  pages in all, shared by image, stack and the optional app arena.

### The startup record and the ABI check

At entry, `r0` points to a read-only `sbx_startup` record (`sdk/include/sandbox/abi.h`):
`size`, `major`, `minor`, the boot `generation`, `trial` (1 while the kernel is on trial),
and the quotas `image_bytes`, `stack_bytes` and `handles`. `sp` is the stack top and `lr`
points at a page that turns a return from the entry function into an exit.

**Check `major` and `minor` before using a call that is newer than 1.0.** An older kernel
answers `-SBX_ENOSYS` for calls it does not have. `sdk/lib/crt0.S` calls
`int main(const sbx_startup *)` and uses the return value as the exit status. A short
record is valid; from 1.6 the record grows to 48 bytes with `app_generation`, and
`sbx_app_generation()` checks size and version for you.

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
`sdk/include/sandbox/abi.h`. Minors only add. Every call returns 0 or a negative `SBX_E*`
value; no call numbers or records ever change. (The first row lists the ABI 1.0 draft
described in `docs/design.md`.)

| ABI | Added |
| --- | --- |
| 1.0 | Calls `open`, `read`, `write`, `close`, `ioctl`, `mmap` (only the framebuffer), `exit`, `getdents` (call 11), `health`. Read-only files under `/sd` (the card's root). `/dev/log` (each write is a log line of up to 120 characters, shown as `OS: ...` on USB). `/dev/fb0`: the surface starts as **640x480x32** XRGB8888, `mmap` it, draw, then `FB_PRESENT` a rectangle (ioctl class 1). `/dev/input/event0`: 16-byte Linux-style events, opened read-only with `SBX_O_NONBLOCK`; `INPUT_QUERY` and `INPUT_STATE` (class 3). **Today the only source is a synthetic device fed over USB** (`KEY`, `AXIS`, `INPUT`). |
| 1.1 | `clock_gettime` (call 13, `sbx_clock_ns`: monotonic nanoseconds, the same clock as input events) and `sleep_until` (call 14). |
| 1.2 | `/dev/fb0` modes: `FB_SET_MODE` (width a multiple of 4, any mode that fits the 1,228,800-byte surface, in XRGB8888, RGB565 or 8-bit indexed) and `FB_PALETTE` for 8-bit. **960x540x16** fits and is exactly half of 1080p. 1920x1080 does not fit. |
| 1.3 | `seek` (call 9): absolute offsets 0 through end of a regular `/sd` file. |
| 1.4 | `getdents_meta` (call 18: size, FAT date/time words, first cluster). Bounded **cache** writes under `/sd/OS/CACHE/`: `fsync` (call 12) and copy-publish `rename` (call 19); see `docs/cache-storage.md`. |
| 1.5 | `getdents_long_meta` (call 20): validated VFAT long names as UTF-8; use a buffer of at least 280 bytes. Calls 11 and 18 keep returning 8.3 aliases. |
| 1.6 | Opt-in **app arena** (call 21): one fixed block of 2,195,456 bytes at `0x30000000`, read/write, never executable, granted once per run. See `docs/app-arena-abi.md`. |
| 1.7 | Save folders: write scope `/sd/OS/SAVES/<GAME>/<NAME>` (`EDQUOT` -122 past 64 game folders or 32 files per game); see `docs/saves-storage.md`. |

**Not yet:** sound (no `/dev/audio`; a PCM host core exists in `docs/audio-host-core.md`
but is not linked into the kernel), a real USB keyboard or gamepad (input is synthetic
only), Wi-Fi and Bluetooth (no wireless driver runs), HDMI audio and HDMI-CEC, and the
planned `/state` journal store. Anonymous `mmap`, free or resize of the arena, and
general SD writing are not supported either.

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
1 to 8 of `A-Z 0-9 _ -`, an optional extension of up to 3. Your program sees long names
only through call 20 (ABI 1.5); calls 11 and 18 return the 8.3 alias. A full path,
including `/sd`, must fit in 255 bytes.

**What a program may write.** Only `/sd/OS/CACHE/<NAME>` and `/sd/OS/SAVES/<GAME>/<NAME>`:
upper-case 8.3 names, no `..`, no extra directory levels. Open with
`SBX_O_WRONLY|SBX_O_CREAT|SBX_O_TRUNC`, write up to 1 MiB, then `sbx_fsync` to commit; close
without fsync **discards** the data. One writer at a time. Rename inside the folder is
copy-then-publish, so a save written as `NAME.TMP` and renamed survives power loss as the
old file or the new one. Everything else on the card is read-only (`EROFS`); this is not
a journal, and a power cut can leave an orphan `.TMP` file.

## 6. Developing over USB

Plug the Pi's USB data port into your computer. It appears as a serial device
(`/dev/cu.usbmodem*` on macOS). `scripts/usb_kernel.py <port> <command>` (Python 3
standard library only) talks to it. Each command runs an `INFO` handshake first.

| Command | What it does |
| --- | --- |
| `info` | print the device's `INFO` line: slots, generations, `updates=ready\|locked`, `os=running\|stopped\|none`, features |
| `tail` | print the kernel log (`LOG ...` lines) until you stop it |
| `put FILE [--name NAME]` | install FILE as `/OS/<NAME>` (upper-case 8.3, 1 byte to 1 MiB, CRC and SHA-256 checked) |
| `putgame FOLDER DIR` | install every file as `/GAMES/<DIR>/<NAME>` |
| `putapp FOLDER DIR` | install every file as `/OS/APPS/<DIR>/<NAME>` |
| `run NAME [--seconds N]` | start `/OS/<NAME>` now and show the log; starts a fresh fault budget and cancels autostart |
| `stop` | stop the OS program at a safe boundary; the recovery console stays |
| `key NAME...` | press keys on the synthetic input device: `up down left right enter esc q space`, a Linux code, or `NAME:down` / `NAME:up` |
| `axis CODE VALUE` | set an axis on the synthetic device |
| `input add\|remove` | plug or unplug the synthetic device |
| `send KERNEL.SKI` | update the kernel itself (A/B slot, trial boot) |

**The `INFO` fields:** `updates=ready` means this kernel accepts update requests (a capability);
`updates=locked` means it does not; `updates=busy` means a new kernel is already pending, so wait
for its confirmation. `os=running` is an OS program running now, `os=stopped` one ran and was
stopped, `os=none` none has run since boot. New code should read `updates=` for kernel
update readiness and `os=` for program state.

`features=` is **legacy**, with its bytes preserved for old Mac senders. Its tokens mean:

| Token | Legacy meaning |
| --- | --- |
| `kernel` | Kernel updating is enabled; emitted together with `no-os` |
| `no-os` | Historical marker accompanying kernel update readiness; it does **not** mean no OS program exists or is running |
| `locked` | Kernel updating is disabled; replaces the entire `kernel,no-os` value |

In `core/protocol.c`, `updates_ready` selects exactly `features=kernel,no-os` or
`features=locked`. The legacy value does not report program state or distinguish a
pending update: it can accompany `os=running` or `updates=busy`.

**When `PUT2` is admitted:** `INFO` was sent this session, storage is ready, `updates=ready`
(not locked) and no update slot is pending. Otherwise `ERR not-ready`. `os=running` alone does
not block a kernel update. `usb_kernel.py send` parses `INFO` into exact fields and refuses
before sending if `updates=` or `pending=` repeats, `updates=` is not exactly `ready`
(including `busy`), or a `pending=` field is not a plain decimal 0. When `updates=` is
present, it is authoritative and `features=` is ignored. With no `updates=` field,
the fallback for old kernels accepts only one exact legacy `features=kernel,no-os`.
`os=` is parsed independently and does not gate kernel updates. The web page (`web/src/piRemote/protocol.ts`, in
the GameMaker repo) reads `INFO` as a generic map and never authorizes updates.

**The not-ready rule:** `PUTF` (and so `put`, `putgame`, `putapp`) is refused with
`ERR not-ready` while an OS program is running, and during a kernel update. **Run `stop`
first**, then install, then `run`. If you are fast, you can also install during the first
10 s after boot, before autostart. Install a new `JS.ELF` or `GMOS.ELF` the same way.

## 7. Make your own JavaScript OS

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
- **Handles:** 16 at a time; one `/dev/fb0` handle.
- **Check your minor version.** A kernel that is older than your SDK returns `ENOSYS`.
- **Framebuffer size:** a program starts in 640x480x32. A mode that does not fit the
  surface is refused.
- **Transfers:** at most 1 MiB per read or write call.
- **Documentation drift:** `tests/test_docs_abi.py` fails when `SBX_ABI_MINOR` in
  `abi.h` is newer than the last row of the table in section 4. Add the row when you add
  a minor.

Further reading: `docs/design.md` (the original design; parts are still plans),
`docs/m1.md` (what was built, in order), `docs/syscall-acceptance.md`, `sdk/README.md`,
and `programs/diag` for a small example program.
