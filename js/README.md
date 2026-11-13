# Sandbox JS

Working name. A layer on top of sandbox-kernel so you can write your own OS for the Pi Zero W **in JavaScript alone**: no C, no cross compiler, no kernel source.

You write apps in JavaScript. Sandbox JS is one ELF (QuickJS-ng inside) that the kernel boots like any program; it turns the kernel's small system-call surface (files, display, input, audio, e-ink, serial, power) into plain JavaScript objects, plus a DOM-lite UI profile (elements, a small CSS subset, a 2D canvas) for people who want a page-shaped shell. Your OS is a folder of `.js` files copied to the card.

**Status: slice 0 of 7.** Only the contract exists: [docs/contract.md](docs/contract.md) lists every kernel call and what it becomes in JavaScript, with the gaps. No runtime code lives here yet. The JS that runs today is `JS.ELF` in the `gamemaker-os` repository ([its guide](https://github.com/lewismoten/gamemaker-os/blob/develop/docs/js.md); the kernel's [author guide](../docs/os-authors.md) section 7 and [spike](../docs/js-spike.md)).

## What v1 is, and is not

- Trusted maker apps. It does not promise to protect the card or other apps from hostile code.
- One ELF under 1 MiB, with an always-embedded recovery picker.
- ABI 1.12 kernels, with graceful fallback on older ones.
- Game-Maker play (the `game` element) is a separate profile owned by gamemaker-os.

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

## Slice 1 needs

1. A decision on the `JS.ELF` source move: copy from gamemaker-os with history or a clean import, and which repository builds `JS.ELF` afterward.
2. QuickJS-ng pinned (version and license text) and its build ported to this repository's Makefile and SDK.
3. Notices for QuickJS-ng, newlib pieces and the bundled pixel fonts, kept in NOTICE.md.
4. The fixtures that prove "existing behavior unchanged" (HELLOPI and the benchmark apps) and a host test harness that runs them against the kernel's host model.
5. ELF and PT_LOAD size reports in CI-free local scripts.

## Checking the contract

```sh
python3 js/tools/contract.py check   # schema, every syscall and ioctl covered, ABI minor current
python3 js/tools/contract.py md      # print the tables for docs/contract.md
```
