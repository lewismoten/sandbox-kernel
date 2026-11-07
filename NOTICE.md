# Notices and provenance

Copyright (c) 2026 Lewis Moten. Original material in this repository is licensed
under the [MIT license](LICENSE).

AI models used on this repository:

| Model | Tool | What it did |
| --- | --- | --- |
| GPT-6.1 Sol (OpenAI) | Codex CLI | Researched license options; wrote the first design, milestones and contribution rules (2026-11-06) |
| Claude Opus 5.5 (Anthropic) | Claude Code | Directed the work; reviewed and edited the design; added profiles, gates and latches; edited docs (2026-11-06) |

This repository currently contains documentation only. No third-party source, firmware, or binaries are included. The dependency
table in [docs/design.md](docs/design.md#dependencies-and-primary-references)
identifies planned sources; it is not a claim that they have been imported,
pinned, ported, or tested.

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
