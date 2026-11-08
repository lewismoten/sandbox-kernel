# Notices and provenance

Copyright (c) 2026 Lewis Moten. Original material in this repository is licensed
under the [MIT license](LICENSE).

AI models used on this repository:

| Model | Tool | What it did |
| --- | --- | --- |
| GPT-6.1 Sol (OpenAI) | Hermes Agent (openai-codex) | Original unlinked bounded PCM host core, native/ARM qualification, rejection/model tests and documentation (2026-11-08); no HDMI driver or audio ABI |
| GPT-6.1 Sol (OpenAI) | Hermes Agent (openai-codex) | VFAT long-name reader, Unicode simple folding, ABI 1.5 opt-in enumeration, compatibility and malformed-media tests (2026-11-08) |
| GPT-6.1 Sol (OpenAI) | Codex CLI | Researched license options; wrote the first design, milestones and contribution rules (2026-11-06) |
| GPT-6.1 Sol (OpenAI) | Hermes Agent (openai-codex) | M1 host acceptance: deterministic ELF rejection witnesses and extended fuzzing, syscall edge cases, resource exhaustion and USB queue reserves; Linux host binding and FAT image label regressions; integration and coverage documentation (2026-11-07) |
| GPT-6.1 Sol (OpenAI) | Hermes Agent (openai-codex) | M0 deterministic control-record, FAT32, image-manifest and USB sender/receiver property tests; coverage matrices and combined host verification (2026-11-07) |
| GPT-6 (OpenAI) | Codex | Implemented original M0 code, integration, tools, host checks and documentation (2026-11-06) |
| Claude Opus 5.5 (Anthropic) | Claude Code | Directed the work; reviewed and edited the design; added profiles, gates and latches; edited docs (2026-11-06) |
| Claude Sonnet 5.5 (Anthropic) | Claude Code | First-board fixes: mailbox and SD handoff, failure codes, panel rotation, 5×7 font, logo badge and its generator, host screen renderer (2026-11-07) |
| Claude Opus 5.5 (Anthropic) | Claude Code | M1 slice 1: multi-tag mailbox, HDMI framebuffer and test pattern, read-only FAT32 assets, boot probes, SDK headers and ABI checks, host tests and FAT test images, board runs (2026-11-07) |
| Claude Opus 5.5 (Anthropic) | Claude Code | M1 slice 2: User mode entry and exceptions, user page tables, ELF32 checks and fuzz tests, system-call dispatcher, OS supervisor and restart policy, /OS file install over USB, SDK crt0 and link map, DIAG and fault programs, board runs (2026-11-07) |
| Claude Opus 5.5 (Anthropic) | Claude Code | M1 slice 3: input event queue and synthetic USB source, getdents, health heartbeat, PUTF into /GAMES folders with folder growth, host tests, board runs (2026-11-07) |
| Claude Opus 5.5 (Anthropic) | Claude Code | M1 slice 4: clock_gettime and sleep_until (ABI 1.1), SDK records and wrappers, host tests, board run (2026-11-07) |
| Claude Sonnet 5.5 (Anthropic) | Claude Code | JS spike: PUTF to 1 MiB and /OS/APPS folders, ABI 1.2 screen modes and palettes, the boot clock, branch prediction and write-back user pages, host tests, board benchmarks, docs/js-spike.md (2026-11-07) |

The initial design was documentation only. M0 now fetches the pinned dependencies
listed below into gitignored third_party/ and generates boot artifacts. The dependency
table in [docs/design.md](docs/design.md#dependencies-and-primary-references)
identifies planned later sources; only the M0 inventory below describes imported inputs.

Hardware and protocol observations came from a separate throwaway prototype built on Circle, which
booted on a Raspberry Pi Zero W v1.1 with a Waveshare 2.13 inch e-Paper HAT (V4): e-ink status with
partial refresh, a USB serial link to a Mac, and kernel updates over that link. The prototype was read
for facts only. Its code is not included or translated.
Circle, by R. Stange and contributors, is GPL-3.0-or-later and is not a dependency.

Credit the independent sources when their code is actually imported:

- Ha Thach and TinyUSB contributors: MIT USB stack.
- ChaN: FatFs, with its own permissive BSD-style notice.
- Waveshare team: V4 e-paper driver with an MIT permission notice.
- Swedish Institute of Computer Science and lwIP contributors: BSD-3-Clause.
- John Cronin: MIT SD controller work, if selected directly from upstream.
- SDL authors including Sam Lantinga: zlib-licensed controller work, if selected.
- OpenBSD contributors: per-file ISC/BSD radio driver candidates, if selected.
- Zephyr contributors: Apache-2.0 Bluetooth candidates, if selected.

Broadcom/Raspberry Pi hardware documentation and Arm architecture manuals are
technical references; their text is not relicensed under MIT. Linked sources
remain under their own terms. Raspberry Pi boot/GPU firmware and wireless
firmware are separate vendor artifacts with their own redistribution conditions.
Future releases must ship the exact notices required by the files they contain;
this credit list does not replace those notices.

## M0 implementation, 2026-11-06

Original startup, BCM2835 GPIO/UART/timer/interrupt/SPI/EMMC/mailbox/watchdog
backends, renderer/font, SHA-256/CRC, restricted FAT writer, boot selector,
update parser and Python tools were written here under MIT. No prototype Circle
sources were read for this implementation. No Circle artifacts are linked or
shipped. No FatFs code is used: the original writer operates only on preallocated
FAT32 file data sectors. No libc or libgcc runtime is linked; unsigned EABI
division and memory/string helpers are original MIT implementations.

TinyUSB **0.18.0**, Ha Thach and contributors, is fetched directly from
<https://github.com/hathach/tinyusb/releases/tag/0.18.0>. Archive SHA-256:
`e7fa1bd723213749a0362c79eaccc99e84c8adea8f0a63588c4e4812608b7aa9`.
Only the core/common/device/CDC/OS-none/DWC2 source subset is retained by the
fetch script; no upstream BSP or SDK is used. The sources are unmodified.
Original `include/broadcom/` headers satisfy the BCM port's glue interfaces.
The MIT license and per-file notices, including William D. Jones, Jan Duempelmann,
Reinhard Panhuber and other listed authors, are retained in
[licenses/TinyUSB.txt](licenses/TinyUSB.txt), and in fetched sources. Fetching
writes `third_party/tinyusb/SHA256.json` with every retained file's hash.

V4 reset/register/waveform facts and sequence were adapted independently from
Waveshare team's `EPD_2in13_V4.c` (V1.1, 2021-10-30) at commit
`d28b3c4cfc78eef1c6cd6e6e3013654e449d2864`:
<https://github.com/waveshareteam/e-Paper/blob/d28b3c4cfc78eef1c6cd6e6e3013654e449d2864/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_2in13_V4.c>.
Source SHA-256: `ec998b30bb9d28662e23f2dee64d609ed8483824270d3e9c5cd86993f202dfa4`.
The full permission/warranty notice is retained in
[licenses/Waveshare.txt](licenses/Waveshare.txt). The SPI adapter, cooperative
state machine, pixels and glyphs are original; upstream fonts are not imported.

Boot firmware is pinned to Raspberry Pi firmware tag **1.20240902**, commit
`b5eb52b343e9bc1391a8059a38e12e470c109f5c`. Only `bootcode.bin`, `start.elf`,
`fixup.dat` and matching `LICENCE.broadcom` are fetched/shipped. Their exact
SHA-256 values are checked in at `scripts/firmware-lock.json`. Vendor firmware
is separately licensed, not MIT. No DTB, Linux kernel, radio blob or GPL runtime
is shipped. The generated boot image includes the matching original vendor
license, MIT license and TinyUSB/Waveshare notices. The cross compiler is a build
tool, not shipped firmware; no toolchain code is included in the linked images.

Primary technical references used:

- Broadcom **BCM2835 ARM Peripherals, 06 February 2012**: chapters 1 (addressing
  and ordering), 5 (EMMC), 6 (GPIO alternate functions), 7 (interrupts), 10 (SPI0),
  12 (system timer), 13 (PL011 UART), 15 (USB). The manual's USB chapter is not a
  complete DWC2 specification; TinyUSB supplies the controller implementation.
- Arm **DDI 0301H**, ARM1176JZF-S r0p7 TRM: chapter 3 CP15, cache/TLB maintenance,
  control register and ARMv6 translation descriptors; exception modes/stacks.
- Raspberry Pi firmware documentation, `config.txt`/legacy boot configuration and
  [mailbox property interface](https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface)
  (page revision dated 2023-12-07, consulted 2026-11-06): ARM memory, UART/EMMC
  clock IDs, USB/SD power devices, mailbox channel 8.
- Adafruit `broadcom-peripherals` MIT SVD register descriptions, Scott Shawcroft,
  commit `0322a9e7c279d8d85d5facaa41ce8439fa5d0dbc`,
  `svd/peripherals/bcm_power_manager.svd.jinja`: independent verification of
  watchdog RSTC/WDOG offsets, password, full-reset field and 20-bit counter.
  No code imported. Reset-status offset and watchdog timebase require board
  measurement; PM is not described fully in the public BCM2835 manual.
- Standard SD CMD0/2/3/7/8/9/13/17/24/55/ACMD41 command semantics. The public
  manual errata page at elinux.org was inaccessible (access denied) during this
  implementation; EMMC errata and physical behavior remain qualification work.
- The Game-Maker: Sandbox Edition logo (sandbox, sand castle, pail and shovel):
  the same author and MIT license as this project. `scripts/make_logo.py` redraws
  its subject as 1-bit art (`core/logo_data.c`); the panel font in `core/render.c`
  is original artwork. The drawing script was written with Claude Sonnet 5.5
  (Anthropic).

Build results and limitations are recorded in [docs/m0.md](docs/m0.md).
M0 acceptance requires real-board and power-interruption testing.

## VFAT Unicode data

`scripts/data/CaseFolding-16.0.0.txt` is the Unicode Consortium's Unicode 16.0.0
case-folding data, retrieved from
<https://www.unicode.org/Public/16.0.0/ucd/CaseFolding.txt>.
SHA-256: `6f1f9c588eb4a5c718d9e8f93b782685e5c7fec872cf05e8e6878053599e09bb`.
The C+S simple mappings generate `include/vfat_fold.h` through the original MIT
script `scripts/unicode_fold.py`; the generated data remains Unicode-3.0 licensed.
The full Unicode License V3 is in [licenses/Unicode.txt](licenses/Unicode.txt),
copied to `UNICODE.TXT` in boot distributions. The original VFAT parser imports
no FatFs or other filesystem implementation.
