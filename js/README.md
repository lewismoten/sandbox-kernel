# Sandbox JS

Working name. A layer on top of sandbox-kernel so you can write your own OS for the Pi Zero W **in JavaScript alone**: no C, no cross compiler, no kernel source.

You write apps in JavaScript. Sandbox JS is one ELF (QuickJS-ng inside) that the kernel boots like any program; it turns the kernel's small system-call surface (files, display, input, audio, e-ink, serial, power) into plain JavaScript objects, plus a DOM-lite UI profile (elements, a small CSS subset, a 2D canvas) for people who want a page-shaped shell. Your OS is a folder of `.js` files copied to the card.

**Status: slice 2 of 7.** The contract ([docs/contract.md](docs/contract.md)) lists every kernel call and what it becomes in JavaScript, with the gaps. The runtime now lives here too: a clean import of `JS.ELF` from `gamemaker-os` ([IMPORT.md](IMPORT.md) has the source commit, every change and what slice 2 must split), with no API expansion. `gamemaker-os` keeps its own copy for now ([its guide](https://github.com/lewismoten/gamemaker-os/blob/develop/docs/js.md); the kernel's [author guide](../docs/os-authors.md) section 7 and [spike](../docs/js-spike.md)).

## What v1 is, and is not

- Trusted maker apps. It does not promise to protect the card or other apps from hostile code.
- One ELF under 1 MiB, with an always-embedded recovery picker.
- ABI 1.12 kernels, with graceful fallback on older ones.
- Game-Maker play (the `game` element) is a separate profile owned by gamemaker-os, plugged into the core through [a small C interface](docs/profiles.md). The default build has the no-game core profile, where `<game>` is an inert element.

These are defaults picked for Lewis on 2026-11-13; he can change them (the list is in the contract).

## Slice plan

| Slice | What |
| --- | --- |
| 0 | Contract inventory (this folder): every call, its JS shape, capability, status, test plan, gaps. |
| 1 | Standalone extraction of JS.ELF here: reproducible builds, notices, current apps work, size reported. |
| 2 | Generic core and profile split: no game dependency in the base; the Game-Maker profile stays optional. |
| 3 | Versioning, package manifest, capabilities, path policy, handle accounting, app teardown. |
| 4a to 4d | Files and storage; display, time and input; audio; e-ink and serial. |
| 5 | Execution limits and hardening (script budgets, allocation failure, soak). |
| 6 | Release and maker path: drift checks against the kernel ABI, prebuilt artifacts, a JS-only example OS another author can boot. |
| 7 | Optional adoption by gamemaker-os. |

Design and estimates: the owner's research note `2026-11-10-js-layer-design.md` (kept with Lewis's research folder, not in this repository).

## Build and test

```sh
make -C js           # build/JS.ELF (JS_PROFILE=file.c picks another profile) (stripped), build/JS.full.elf; needs the Arm GNU Toolchain
make -C js host      # build/host/jsrun, the same runtime over stdio
make -C js test      # host cases against tests/fixtures.json (log lines and framebuffer CRC-32 match the gamemaker-os build), then the profile interface tests (hook order, key routes, 50 start/stop cycles with a flat heap)
make -C js size      # ELF bytes and each PT_LOAD against the 1 MiB ceiling and the 8 MiB image quota
make -C js repro     # two clean builds at different paths must match byte for byte
```

Current size (2026-11-13, toolchain 15.3.rel2, core profile): `JS.ELF` 752,220 bytes (71.7% of 1 MiB, 296,356 spare); two PT_LOADs, R-X 744,920 bytes and RW- 2,324 file bytes with 3,960,332 in memory (the 3 MiB heap, a 256 KiB read buffer and QuickJS's data); loaded image 4.49 MiB of the 8 MiB quota. Run on the host, not yet on a board. Third-party notices: [third_party/README.md](third_party/README.md). Nothing here is flashed by the tests.

## Slice 1 needs (done)

1. A decision on the `JS.ELF` source move: copy from gamemaker-os with history or a clean import, and which repository builds `JS.ELF` afterward.
2. QuickJS-ng pinned (version and license text) and its build ported to this repository's Makefile and SDK.
3. Notices for QuickJS-ng, newlib pieces and the bundled pixel fonts, kept in NOTICE.md.
4. The fixtures that prove "existing behavior unchanged" (HELLOPI and the benchmark apps) and a host test harness that runs them against the kernel's host model.
5. ELF and PT_LOAD size reports in CI-free local scripts.

## Slice 2 (done), and what slice 3 needs

The core no longer has a game dependency: `JS_GAME` is gone, replaced by `struct jsrt_profile` ([docs/profiles.md](docs/profiles.md)), a no-game core profile and a test profile. Slice 3 (versioning, manifest, capabilities, path policy, handle accounting, teardown) needs: a place in the interface for a profile to declare its capabilities and version (an `abi` field already exists), a handle table the core can count at `jsrt_stop` instead of the test profile's own counters, and a decision on whether profile hooks may read files through a path policy rather than `plat_read`.

## Checking the contract

```sh
python3 js/tools/contract.py check   # schema, every syscall and ioctl covered, ABI minor current
python3 js/tools/contract.py md      # print the tables for docs/contract.md
```
