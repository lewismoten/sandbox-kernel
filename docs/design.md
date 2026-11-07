# Kernel design

Design baseline: November 6, 2026. Everything below is proposed behavior, not
implemented support. The initial target is Raspberry Pi Zero W v1.1. Original
kernel and SDK work is MIT, using route B from Lewis's license decision.

## Ownership and execution model

A small monolithic kernel owns interrupts, MMU tables, timers, DMA, firmware
mailbox access, displays, audio, GPIO, SPI/I2C, SD/FAT, USB, input, and networking.
Drivers stay privileged. The OS program supplies its own interface, application
policy, game logic, asset parsing, decoding, and synthesis. It cannot access
physical registers, DMA descriptors, kernel symbols, or firmware services directly.

Initially one OS program runs on one CPU core. Other cores remain parked.
There is no fork, general process tree, dynamic kernel module loader, or SMP
scheduler. A timer preempts user execution every 10 ms; IRQ handlers acknowledge
hardware and queue bounded work. A kernel dispatcher services queues and resumes
the OS, or idles until a deadline/interrupt when it blocks. Long SD, USB, and
panel operations advance as state machines. No device wait may disable interrupts
for its full duration. Drivers have deadlines, cancellation, and reset policies.
Audio and USB servicing take priority over display refresh and bulk storage.

`spawn` requests an OS replacement through the supervisor; only one OS executes
at a time. The kernel validates and stages the successor before destroying the
current address space. A failed validation leaves the caller running. A successful
handoff destroys its handles and mappings and does not return. Initially the OS
may spawn only manifest-approved images, not arbitrary SD executables. General
multitasking would require a later capability and scheduling design.

## Boot and privilege

The vendor boot chain loads a small original boot selector from FAT. On Zero,
`config.txt` selects this selector as `kernel.img`; this file is not the runtime
kernel slot. The selector initializes enough CPU state, RAM, SD access, serial,
and watchdog support to validate metadata and load a runtime kernel. It passes
a versioned boot record containing board identity, RAM reservations, chosen
kernel/OS generations, reset reason, and trial state. It never trusts an image's
physical addresses without bounds checks. Runtime startup installs vectors and
exception stacks, establishes MMU mappings and caches, starts timers/watchdog,
initializes drivers, then loads the OS. M0 can stop at the kernel recovery console.

CPU startup must handle the actual firmware handoff rather than assume reset
state. Validate incoming boot data, establish a known interrupt/cache/MMU state,
and preserve firmware-owned RAM. Use instruction-cache and TLB maintenance and
required barriers after installing code or changing translations.

| CPU target | Kernel and user execution | Architecture work |
| --- | --- | --- |
| ARM1176, Pi 1/Zero | AArch32 Supervisor mode for kernel; User mode for OS; IRQ, abort, undefined and FIQ modes have protected stacks | ARMv6 page tables and access permissions, CP15 cache/TLB operations; no AArch64 exception levels, Hyp mode, ARMv7, or NEON assumptions |
| Original Pi 2, Cortex-A7 | AArch32 Supervisor/User, with controlled transition from Hyp if entered there | ARMv7 startup, translation and cache/interrupt backend |
| Pi 2 revisions with Cortex-A53; Pi 3/Zero 2 | AArch64 EL1 kernel and EL0 OS for the later native port | Normalize entry from a higher EL if firmware supplies it; no resident hypervisor required |
| Pi 4, Cortex-A72; Pi 5, Cortex-A76 | AArch64 EL1/EL0 | Separate MMU, vectors, interrupt controller and cache support; Pi 5 is AArch64 only |

The ARM1176 reference manual defines its modes and protection machinery;
[Arm DDI 0301H](https://documentation-service.arm.com/static/5e8e294efd977155116a6ca3)
is the starting reference. Later backends require the
[Armv7-A/R manual](https://developer.arm.com/documentation/ddi0406/latest/) or
[Armv8-A manual](https://developer.arm.com/documentation/ddi0487/latest/), plus
the selected core's manual. Implementers must obtain specific revisions; the
web reader could not retrieve the full later architecture manuals during design.

## Memory map and protection

Initial AArch32 virtual layout (4 KiB pages):

| Virtual range | Use |
| --- | --- |
| `0x00000000–0x0000ffff` | Unmapped, including null |
| `0x00010000–0x0fffffff` | Fixed-address OS image segments |
| `0x10000000–0x3fffffff` | Bounded heap and anonymous mappings |
| `0x40000000–0x6fffffff` | Shared display buffers and other approved mappings |
| `0x70000000–0x7ffeffff` | Stack arena; stack grows down with an unmapped guard |
| `0x7fff0000–0x7fffffff` | Reserved, unmapped |
| `0x80000000–0xffffffff` | Privileged kernel, device windows, tables, stacks and high vectors |

These are address reservations, not physical allocation promises. Default OS
resident quota is 64 MiB, stack 1 MiB, and 64 open handles, lowered when available
RAM requires it. No swap or overcommit. Kernel recovery RAM and I/O pools are
reserved before user allocation. AArch64 initially uses the same low user address
ranges with its own privileged high region; it has a separate ELF/SDK profile.

On Zero the traditional firmware load address is `0x8000`, and BCM2835 ARM
peripherals occupy physical addresses starting at `0x20000000`. Those facts do
not define the runtime virtual map. Reserve selector memory until handoff, then
reclaim only explicitly released ranges. Discover usable ARM RAM and firmware/GPU
reservations; also reserve framebuffer memory, boot records and DMA pools.
Do not assume all installed RAM belongs to the ARM CPU. CPU physical and peripheral
bus addresses differ; only board drivers perform checked conversions. See the
[Broadcom address map](https://www.raspberrypi.com/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf).

User code is read/execute; writable data, heap and stacks are non-executable using
the selected CPU's supported translation format. Reject writable/executable
segments and shared-page permission conflicts. Kernel pages, vectors, page tables,
MMIO and DMA buffers are privileged. Do not leave an accessible identity alias.
Use enforced page access permissions, not an ARM domain-manager bypass.

Every syscall checks address arithmetic, mapping permissions, lengths, alignment,
handle generation, and resource limits. Copy input descriptors before acting;
copy output only to writable user ranges. Recoverable faults in copy routines
produce `EFAULT`; unrelated kernel faults trigger panic recovery. DMA always uses
kernel-owned descriptors and bounded buffers. CPU page tables do not constrain
DMA on Zero, so no raw DMA or arbitrary physical `mmap` is available.

## System-call ABI, draft version 1.0

This is a POSIX-style source interface, not Linux binary compatibility or full
POSIX compliance. The MIT SDK will provide C wrappers and translate negative
kernel errors to `-1` plus thread-local or single-thread `errno`. No C++ objects
cross the boundary. Publish SDK headers and binary layout tests before freezing
1.0; the draft numbers below are reserved for that implementation.

For ARM32, execute ARM-state `svc #0` with `r7` = call number, `r0` = pointer to
request record, `r1` = record byte size, and `r2` = ABI major (1). `r3–r6` must be
zero. Return signed status in `r0`: zero on success or negative errno. Results,
including 64-bit counts, go in the record. Preserve `r4–r11` and SP; `r1–r3`,
`r12`, LR and condition flags are caller-clobbered. The kernel restores a valid
user execution state on exception return. ARMv6 code uses soft-float AAPCS32
initially; Thumb entry and user VFP are not supported in the first profile.

For AArch64 use `svc #0`, `x8` = number, `x0` = record pointer, `x1` = size,
`x2` = major, `x3–x7` = zero; status returns in `x0`. Preserve `x19–x29` and SP;
other argument/scratch registers, LR and flags are caller-clobbered. Both
profiles use little-endian memory. No arguments are passed on the syscall stack.

Every record starts with four `u32` fields: `size`, `flags`, `reserved0`,
`reserved1`. Its size must match the register size and meet that operation's
minimum. Flags/reserved fields are zero unless documented. Payload fields occupy
successive 8-byte slots, with explicit padding rather than compiler packing.
Addresses and lengths are `u64` even on ARM32; upper address bits must be zero
there. Signed values are `i64`. Counts, offsets, and times never depend on C
`long` or `time_t`. Records have 8-byte alignment. Inputs and outputs are listed
in order below; unused output fields are zero on entry. No output is valid on
error except where a later explicit extension says otherwise.

| Number | Call | Payload slots, inputs then outputs | Semantics |
| --- | --- | --- | --- |
| 0 | `query` | selector, buffer address, capacity; required bytes | Version, limits, feature list, or device inventory |
| 1 | `open` | path address, path bytes, open flags, mode; fd | Absolute UTF-8 path; explicit length, no embedded NUL |
| 2 | `read` | fd, buffer, count; transferred | Short reads allowed; zero means EOF |
| 3 | `write` | fd, buffer, count; transferred | Short writes allowed; success does not imply durability |
| 4 | `close` | fd | Release handle and cancel its outstanding operations |
| 5 | `ioctl` | fd, request, buffer, bytes | Sized, device-class-specific record; never an untyped register operation |
| 6 | `mmap` | fd, offset, length, protection, map flags; address | Kernel chooses address; anonymous or approved shared buffer only |
| 7 | `munmap` | address, length | Whole mapping release initially |
| 8 | `poll` | array address, entry count, deadline ns; ready count | Absolute monotonic deadline; zero is immediate, all-ones is infinite |
| 9 | `seek` | fd, signed offset, whence; new offset | Regular files only; devices return `ESPIPE` |
| 10 | `stat` | fd, output buffer, bytes | Sized type/size/mode record, no host `struct stat` |
| 11 | `getdents` | fd, buffer, bytes; transferred | Directory records with length, type, name length, UTF-8 name and 8-byte padding |
| 12 | `fsync` | fd | Complete the supported object's durability operation |
| 13 | `clock_gettime` | clock selector; nanoseconds | Selector 0 is monotonic time since kernel start |
| 14 | `sleep_until` | deadline ns | Sleep without busy waiting |
| 15 | `spawn` | path address, path bytes, argument address, argument bytes | Replace current OS through supervisor; argument blob limited to 4 KiB |
| 16 | `exit` | signed status | Does not return; supervisor applies restart policy |
| 17 | `health` | boot generation, sequence | OS readiness/heartbeat report; cannot alone confirm a kernel trial |
| 18 | `replace` | path address, path bytes, data address, data bytes, expected generation; new generation | Atomic settings/save replacement through durable store |

`query` selectors: 0 = ABI/build/architecture information, 1 = limits, 2 =
feature records, 3 = device records. Query output starts with `u32 size`,
`u16 major`, `u16 minor`; following records are length-delimited with stable
numeric type IDs. If capacity is insufficient, return `ENOSPC` and the required
bytes as the sole defined error output. Unknown selectors return `EINVAL`.
Capabilities enumerate operations, board-supported device classes, limits,
USB role, PCM formats, display modes, and granted rights. Enumeration is a
snapshot; device disappearance is reported through poll and subsequent calls.

ABI major mismatch returns `EPROTONOSUPPORT` and prevents loading an incompatible
OS. Minors add fields/calls/features; existing field meanings and numbers never
change. Old records are accepted; missing trailing fields default to zero.
Unknown trailing nonzero fields/flags are rejected. Unknown calls return `ENOSYS`;
unsupported device requests return `ENOTTY`. A newer OS must require only features
reported by the running kernel. Feature availability is separate from privilege:
handles receive manifest-limited rights, never greater rights through `ioctl`.

Base errors use these fixed POSIX-style values: `EPERM=1`, `ENOENT=2`, `EIO=5`,
`EBADF=9`, `EAGAIN=11`, `ENOMEM=12`, `EACCES=13`, `EFAULT=14`, `EBUSY=16`,
`EEXIST=17`, `ENODEV=19`, `ENOTDIR=20`, `EINVAL=22`, `EMFILE=24`, `ENOTTY=25`,
`ENOSPC=28`, `ESPIPE=29`, `EROFS=30`, `EPIPE=32`, `ENAMETOOLONG=36`,
`ENOSYS=38`, `EPROTONOSUPPORT=93`, `ETIMEDOUT=110`. No signals initially.
Transfers larger than 1 MiB are rejected; paths are at most 255 bytes and poll
arrays at most 64 entries. An I/O error after some bytes transferred returns a
successful short count; a later call reports the remaining error.

Open flags: read-only 0, write-only 1, read/write 2, nonblocking `0x100`;
unsupported bits fail. Creation through `open` is initially unsupported; mode
must be zero. `seek` origins are start 0, current 1, end 2. Mapping protections
are read 1, write 2, execute 4; map flags are shared 1 or private 2, with anonymous
`0x20` and fd = all-ones. No fixed-address, file-executable, or writable/executable
mapping. Anonymous mappings are private. `poll` entries are `{u32 fd, u16 events,
u16 revents}` with readable 1, writable 4, error 8, hangup 16, invalid 32.
Invalid handles set the invalid bit. Timeouts return success with zero ready
entries. Nonblocking operations return `EAGAIN` when no progress is possible.
Device removal sets hangup/error; stale handle IDs cannot refer to new devices.

## Program-visible device tree

This is the VFS namespace, distinct from a firmware flattened device tree.
The kernel consumes board descriptions; programs see generic class records.
Missing hardware means an absent node or `ENODEV`, never fabricated support.

| Path | Contract |
| --- | --- |
| `/dev/fb0` | Mode query reports dimensions, pitch, pixel format and buffer bytes. `mmap` exposes a bounded RAM drawing surface; present ioctl copies/flips through the driver. Status rendering can reclaim output after an OS fault. No arbitrary physical framebuffer access. |
| `/dev/audio` | PCM format query/configure, bounded queued writes, drain/stop and underrun query. Start with signed 16-bit little-endian mono PCM through PWM; rate is negotiated. Silence on underrun. HDMI is a later backend. |
| `/dev/input/eventN` | Linux-style event types/codes for keys, buttons, axes and synchronization; capability/axis queries and per-open queues. Keyboard, pad, IR and CEC drivers normalize here. |
| `/dev/spi0` | Kernel-managed SPI messages: device selector, mode, speed, bounded TX/RX buffers. Only manifest-granted chip selects; no controller registers. |
| `/dev/gpio` | Request/release permitted lines, read/write values and edge events. Kernel claims e-ink, storage, audio and radio pins first; programs cannot steal them. |
| `/dev/i2c0` | Optional bounded kernel-managed bus transfers to granted addresses. |
| `/dev/epaper0` | Geometry/status query, bitmap submission, full/partial refresh and completion polling. Kernel can override content for faults. |
| `/dev/ttyGS0` | CDC-ACM byte stream in USB device mode. Kernel owns enumeration, logging and update parsing; an optional OS console channel is framed and demultiplexed. |
| `/dev/log` | Rate-limited OS log writes; no ability to impersonate kernel severity/source. |
| `/sd/...` | FAT assets and directories, initially read-only. `/sd/games` is an ordinary content directory, not a kernel-specific format. |
| `/state/settings/...`, `/state/saves/...` | Logical durable files, read through normal handles and updated through `replace`; per-OS quotas and path ownership. |

Input records have a fixed 16-byte layout: `u64 monotonic_ns`, `u16 type`,
`u16 code`, `i32 value`. Numeric event conventions follow Linux input UAPI
semantics, independently declared in the SDK; this is not Linux's native
`struct input_event`, whose timestamp layout varies. A read returns whole
records; too-small buffers fail. Overflow emits `SYN_DROPPED`; the OS queries
current key/axis state before consuming a new `SYN_REPORT` group. IDs are not
stable across boots; inventory includes device identity and supported controls.
The semantic reference is the upstream
[Linux input event documentation](https://docs.kernel.org/input/event-codes.html).

Ioctl IDs use `(class << 16) | operation`; classes 1 framebuffer, 2 audio,
3 input, 4 SPI, 5 GPIO, 6 e-paper. Operation 0 is a sized class query; subsequent
operations follow the verbs in the table. Exact class record layouts must be
specified with the implementation before enabling their capability bits.
No driver may invent an undocumented ioctl under ABI 1.0.

### V4 e-ink hardware baseline

The observed HAT wiring uses SPI0 MOSI GPIO10/pin19, SCLK GPIO11/pin23,
CE0 GPIO8/pin24, DC GPIO25/pin22, reset GPIO17/pin11, and BUSY GPIO24/pin18.
The V4 panel is 122 by 250 pixels with a 16-byte row stride (4,000 bytes per
plane); the status surface can rotate to 250 by 122. BUSY is active high.
The prototype uses SPI mode 0 at 4 MHz and a five-second BUSY deadline.
Validate these settings on Lewis's board before claiming support.

Use the independent
[Waveshare V4 driver](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.c)
as the sequence source: hardware reset, BUSY completion, software reset,
controller setup, RAM window/counters, and image-plane writes. Full refresh
establishes both current and previous planes using update control `0xf7` and
activation; partial refresh uses `0xff` and then synchronizes the previous plane.
These are V4 controller facts, not a portable sequence for other revisions.
Implement timing and sleep directly from a pinned upstream version and panel
manual; do not translate the Circle adapter. Panel waits are asynchronous and
bounded. On timeout, disable panel work and report through USB/HDMI/UART.
Refresh cadence, temperature limits, ghosting and panel lifetime require physical
qualification; the prototype's five-second cadence is not a production promise.

## OS image and loader

A release bundle contains a bounded manifest and a native ELF payload. The
manifest records format version, architecture profile, board capability
requirements, ABI major/minimum minor, payload byte count, SHA-256, image/build
ID, memory/handle/time quotas, allowed paths/devices, and recovery policy.
Use an explicitly serialized format, not a dumped C structure; define its exact
wire layout before accepting bundles. Reject missing, duplicate, overflowing,
unknown critical, or inconsistent fields. CRC covers transport; SHA-256 covers
stored content. Neither grants authenticity without a trusted signature.

The initial ELF profile is little-endian ELF32 `ET_EXEC`, `EM_ARM`, ARMv6 ARM
state with EABI5 soft-float calling conventions. AArch64 later uses ELF64
`EM_AARCH64` and a separate artifact. Follow the
[Arm ELF specification](https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst).
Only bounded `PT_LOAD` segments and the agreed entry point are accepted.
No interpreter, shared libraries, unresolved imports, runtime relocations, TLS,
or executable stack. Reject unsupported program headers and incompatible ARM
attributes. Section headers are not required for loading. PIE is deferred.

Validate header/table bounds, integer arithmetic, alignment congruence,
`filesz <= memsz`, file ranges, user virtual ranges, segment/page overlaps,
permissions and quota totals. Entry must be aligned ARM code inside an executable
segment. Ignore ELF physical addresses. Copy segments, zero BSS, allocate guarded
stack/heap, synchronize caches, then return from an exception into User/EL0.
The SDK's `_start` initializes its own runtime. Entry receives a read-only,
versioned startup record pointer in `r0`/`x0`, describing ABI, arguments, boot
trial ID and quotas. Stack is 8-byte aligned on ARM32 and 16-byte aligned on
AArch64. Returning from `_start` goes to a user trampoline that invokes `exit`.
No kernel address is supplied. Loader failure keeps the recovery console alive
and tries the last confirmed compatible OS. Demonstrate a second unrelated OS
program before calling the ABI reusable.

## Faults, watchdog and recovery

User aborts, undefined instructions, invalid syscall buffers, allocation failure,
and heartbeat expiry must not corrupt kernel state. Bad syscall arguments return
errors; a user execution fault terminates the OS. Record build/slot IDs, PC,
mode, exception syndrome/status, fault address and resource usage in a bounded
kernel log. Revoke handles and mappings, stop audio, cancel I/O, reclaim buffers,
and reset device queues before loading another OS.

Default policy: permit one restart of a confirmed OS; a second fault within
60 seconds selects the previous confirmed compatible OS. Any trial OS fault
fails that trial immediately. Without a valid fallback, remain in the kernel
recovery UI with USB updates available. An intentional `exit(0)` returns to
recovery unless a replacement was requested; nonzero exit counts as a fault.

Require an OS readiness report within 10 seconds and a progressing heartbeat
at least every two seconds, including while idle. Timer preemption catches a
runaway user loop regardless of its cooperation. The hardware watchdog is armed
before launching a trial kernel; initial target timeout is eight seconds, subject
to board measurement. Only a health monitor feeds it after observing timer,
dispatcher, USB and storage progress or valid idle states. A user heartbeat alone
cannot feed it. Kernel panic or loss of kernel progress stops feeding it.
Persist the trial attempt before launch so a watchdog reset returns to fallback.

Kernel status takes over HDMI on faults and reserves e-ink error reporting.
USB and UART receive detailed logs; e-ink refresh is rate-limited and cannot
block recovery. A missing display or disconnected USB does not delay fallback.
The intended guarantee is recovery when a valid boot chain and usable media
remain; power loss, failed hardware or corrupt firmware can still require a
card reader. No software can guarantee a visible display after hardware failure.

## SD layout and storage safety

The prototype uses an MBR card with first partition FAT32 (type `0x0c`, at least
128 MiB), vendor boot files at its root, and `/games`; it has no Linux root
filesystem. Preserve that familiar asset layout but do not reuse its FAT rename
update strategy.

Proposed release layout:

| Region | Contents and policy |
| --- | --- |
| Partition 1, FAT32 | Vendor boot files/licenses, explicit per-board selector files/config, manifest, `/games` and read-only assets; mounted at `/sd` |
| Partition 2, fixed raw layout | Two separated boot-control records; per-board kernel A/B payload regions; OS A/B regions with manifests; redundant slot headers and confirmation records |
| Partition 3, durable state store | Two journal/checkpoint banks for settings and saves, exposed at `/state` |

The image builder must publish exact sector offsets, capacities and format
versions. Metadata validates partition bounds and overlap before use. M0 needs
raw block SD access for updates, even though FAT VFS support is an M1 feature.
No host mass-storage export while the kernel owns the card. Firmware, selectors,
partition tables and EEPROM are outside the routine USB update scope.

FAT rename and `fsync` alone are not a power-loss transaction. Keep FAT assets
read-only by default. `replace` writes a complete new logical value plus its
path, length, generation and checksum to the inactive journal space, flushes it,
reads it back, then appends a checksummed commit record and flushes again.
Only a valid complete commit becomes visible. Replay ignores incomplete tails;
compaction copies live values to the other bank and commits a newer checkpoint
before erasing/reusing the old bank. Never overwrite the only valid checkpoint.
Use expected-generation comparison to prevent lost updates. Ordinary `write`
to state files returns `EROFS`; callers use `replace`, with a 1 MiB value limit.
Closing a handle does not commit anything implicitly.

This provides atomic replacement and a journal for settings/saves without claiming
that FAT is journaled. Store quotas reserve space for one complete replacement
and recovery metadata. Flush ordering and card-controller completion are required;
checksums detect torn writes but cannot repair a card that loses both copies.
SD internal flash behavior limits durability guarantees. Qualify cards with
power-cut testing and retain read-only recovery when state is unreadable.

## USB update protocol and automatic fallback

Zero W has one USB data controller. It can be a CDC gadget attached to the Mac
or a host for a wired gamepad, not both at once through a hub. Start with separate
development and play boot configurations, using UART logging in host mode.
Always-connected CDC plus wired input requires another host controller or bridge;
Bluetooth is a later alternative. CDC does not provide SSH or networking.
Use appropriate USB IDs before device distribution. Board connector constraints
follow the [Raspberry Pi hardware documentation](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html).

The new protocol is version 2, deliberately distinct from the prototype's raw
filename replacement. Preserve its simple size+CRC, stop-and-wait, and VERIFY
interaction while implementing a fresh parser and installer:

1. `INFO` returns protocol version, board/architecture, ABI, slot capacities,
   active/confirmed/pending generations and update feature bits.
2. Host sends `PUT2 <kernel|os|bundle> <profile> <decimal-size> <crc32>\n`.
   Fields are bounded ASCII tokens; CRC is eight lowercase hex digits,
   CRC-32/ISO-HDLC as in `zlib.crc32`. No paths or filenames are accepted.
   A bundle can pair a new kernel and OS with explicit compatibility metadata.
3. After target/size/quota checks, reply `READY 1024`. Host sends raw blocks of
   at most 1,024 bytes, waiting for `ACK <cumulative-bytes>` after each nonfinal
   block. The final block receives `VERIFY`. No status text is interleaved with
   binary reception. Length determines framing, including an exact-multiple
   final block. Bound the ASCII command to 256 bytes.
4. Stage the complete transfer in RAM; initial maximum is 2 MiB and minimum
   64 bytes, further limited by advertised capacity and RAM reservation. Validate
   CRC, manifest SHA-256, image format, ABI/profile and compatibility. Reject
   overflow, trailing input and unsupported types. Limits can grow through INFO.
5. Write only inactive slots, flush, read back and verify complete hashes, then
   commit redundant pending metadata. Reply `OK pending rebooting` only after
   durable commit and reboot. Errors use `ERR <code>` and retain confirmed slots.

Fifteen seconds of transfer inactivity, disconnect, or USB reset cancels staging.
A new session begins with INFO; there is no resume or retransmission ambiguity.
After VERIFY the host waits for a longer advertised installation deadline and
must query INFO after reconnect if the final response was lost. Corrupted RAM
staging or an interrupted inactive-slot write never becomes pending. Initially
this is trusted local development: CRC/SHA-256 detect corruption, not malicious
updates. Remote or untrusted updating requires a separately specified signature,
key provisioning and recovery policy before enabling it.

Boot control records carry format/version, monotonically increasing generation,
CRC, confirmed pair, pending pair, attempt-consumed flag and image hashes. An
explicit no-OS sentinel is allowed only for the M0 kernel recovery personality;
from M1 onward a normal boot pair requires a compatible OS. Write
the older record copy and verify it before considering its generation committed;
never invalidate the sole good copy. The selector picks the newest valid record,
validates slot hashes, and **durably consumes the one trial attempt before jumping**.
If that write cannot be verified, do not launch the trial. Booting again without
a valid confirmation chooses the previous confirmed kernel/OS pair, including
after watchdog reset or power loss. A bundle keeps that pair until both new
components are healthy; it does not overwrite either confirmed component.

A trial kernel must service timers/watchdog, storage and recovery USB; in M0
its built-in diagnostics must remain healthy for 30 seconds. From M1 onward,
confirmation additionally requires the OS readiness report and 30 seconds of
continuing kernel/OS progress. A health report is tied to the pending generation.
Commit confirmation durably before clearing pending state. New OS updates can
run with the confirmed kernel; kernel updates retain an ABI-compatible confirmed
OS. Reject updates with no viable compatible fallback pair. A trial may be
abandoned conservatively by an inconvenient power cut; safety takes priority
over retrying it automatically.

If both control records fail, validate both slot manifests and enter recovery
using a valid kernel without marking anything confirmed. If neither kernel is
valid, the fixed selector must provide a minimal CDC/UART recovery path or
explicitly require card-reader rescue; implementing selector CDC recovery is a
separate gate, not assumed from runtime USB support. Routine updates never alter
the selector. Corruption of firmware, FAT boot files or selector still needs
external repair. This replaces the prototype's manual backup-rename recovery;
the Raspberry Pi firmware itself does not manage these A/B trials.

## Board support layering

Keep public VFS/device classes independent of CPU and board definitions:
`core` (handles, supervisor, ABI, recovery), `arch/armv6`, `arch/armv7`,
`arch/aarch64` (entry, traps, MMU/cache), `soc` (interrupts and controller
backends), `boards` (routing, pins, clocks and firmware conventions), and
`drivers` (class implementations). Each board publishes an explicit capability
set. Missing backends fail clearly rather than inheriting guessed addresses.

| Family | Planned port boundary |
| --- | --- |
| Pi 1/Zero/Zero W, BCM2835 | ARM1176 baseline; original timer/GPIO/SPI/PWM/mailbox/SD glue, TinyUSB DWC2 integration; Zero W first acceptance board |
| Pi 2 | Distinguish original BCM2836 Cortex-A7 from later BCM2837 revisions; CPU entry and interrupt/routing differences; one core initially |
| Pi 3/Zero 2 W | BCM2837-class AArch64 CPU backend, board-specific wireless/UART routing and USB connector exposure |
| Pi 4/400, BCM2711 | Separate interrupt, SD, address/cache/DMA and display backend; USB-C device route differs from USB-A xHCI host |
| Pi 5, BCM2712/RP1 | New AArch64 platform work, PCIe/RP1 GPIO/SPI/USB/storage paths; no assumed exposed Zero-style USB gadget route |

Hardware family facts come from the
[official processor documentation](https://www.raspberrypi.com/documentation/computers/processors.html).
A multi-board card will carry distinct selector/kernel artifacts and explicit
firmware configuration. Filenames do not prove compatibility. Qualify actual
board revisions and Compute Module carriers separately. Keep Pi 4/5 EEPROM and
vendor firmware changes outside normal runtime updates.

## Dependencies and primary references

No dependency is vendored yet. Links below identify intended sources, not pinned
release inputs. Before import, pin revisions, retain full notices and audit the
complete selected dependency set, including build/runtime support libraries.
The MIT project license does not replace upstream terms.

| Component/source | License or terms | Planned use and limit |
| --- | --- | --- |
| Original kernel and SDK | MIT, Lewis Moten | Startup, protection, ABI, supervisor, drivers and recovery written here |
| [Broadcom BCM2835 manual](https://www.raspberrypi.com/app/uploads/2012/02/BCM2835-ARM-Peripherals.pdf), Raspberry Pi and Arm manuals linked above | Copyrighted technical references | Hardware facts; no copied manual prose/code; consult errata and specific core manuals |
| [TinyUSB](https://github.com/hathach/tinyusb), [license](https://github.com/hathach/tinyusb/blob/master/LICENSE) | MIT; audit BSP dependencies | CDC-ACM/DWC2 device first, HID host later; board glue and real Zero testing required |
| [FatFs](https://elm-chan.org/fsw/ff/), [terms](https://elm-chan.org/fsw/ff/doc/appnote.html#license) | ChaN's permissive BSD-style notice, retain exact text | FAT asset access; supplies neither SD controller nor power-loss journaling |
| [Waveshare V4](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.c) | MIT permission notice in source | Panel sequence only, with original kernel SPI/GPIO adapter |
| [lwIP](https://github.com/lwip-tcpip/lwip), [license](https://github.com/lwip-tcpip/lwip/blob/master/COPYING) | BSD-3-Clause | M3 IP stack; does not supply Wi-Fi, TLS or SSH |
| [John Cronin SD work](https://github.com/jncronin/rpi-boot) | MIT candidate; verify chosen files | Optional upstream SD reference; no extraction from Circle or GPL SDHOST |
| [SDL](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt) | zlib | Optional independently obtained DualSense report reference, not a Linux driver port |
| [OpenBSD bwfm](https://cvsweb.openbsd.org/src/sys/dev/ic/bwfm.c) | Per-file ISC/BSD, audit frontend/dependencies | M3 radio candidate; no verified bare-metal CYW43438 port yet |
| [Zephyr](https://github.com/zephyrproject-rtos/zephyr/blob/main/LICENSE) | Apache-2.0 baseline; per-file audit | M3 Bluetooth candidate; Classic HID and runtime dependencies must be proven |
| [Raspberry Pi boot firmware](https://github.com/raspberrypi/firmware/blob/master/boot/LICENCE.broadcom) | Separate vendor binary redistribution terms | Required boot artifacts; not MIT; preserve matching notices and DTB licenses |
| Wireless firmware/NVRAM | Supplier/file-specific terms, not selected | Record exact hashes, board calibration source and redistribution terms before shipping |

The linked sources are technical starting points. No GPL kernel/runtime, Circle
component, noncommercial BTstack default license, or unverified driver fallback
is admitted to the permissive baseline. See [NOTICE.md](../NOTICE.md) and
[CONTRIBUTING.md](../CONTRIBUTING.md) for provenance requirements.
