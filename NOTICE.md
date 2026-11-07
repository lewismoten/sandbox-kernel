# Notices and provenance

Copyright (c) 2026 Lewis Moten. Original material in this repository is licensed
under the [MIT license](LICENSE).

OpenAI Codex assisted with research and writing this design, directed by Lewis
Moten, on November 6, 2026. This repository currently contains documentation
only. No third-party source, firmware, or binaries are included. The dependency
table in [docs/design.md](docs/design.md#dependencies-and-primary-references)
identifies planned sources; it is not a claim that they have been imported,
pinned, ported, or tested.

Requirements came from Lewis's local research files:

- `GameMaker/.for-lewis/research/pi-os/kernel-principles.md`
- `GameMaker/.for-lewis/research/pi-os/kernel-license-options.md` (route B)
- `GameMaker/.for-lewis/research/pi-os/answers-2026-11-06.md`

Hardware and protocol observations came from the separate Circle prototype's
README and `src/epaper.cpp`, `src/protocol.h`, and `src/update.cpp` at
`GameMaker/.claude/worktrees/pi-os-bm/hardware/pi-os`.
Those files were read for facts only. Their code is not included or translated.
The prototype's documentation also says physical validation is pending; these
observations are bring-up inputs, not certification of working hardware.
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
