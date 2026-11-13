# Sandbox JS

Working name. A layer on top of sandbox-kernel so you can write your own OS for the Pi Zero W **in JavaScript alone**: no C, no cross compiler, no kernel source.

You write apps in JavaScript. Sandbox JS is one ELF (QuickJS-ng inside) that the kernel boots like any program; it turns the kernel's small system-call surface (files, display, input, audio, e-ink, serial, power) into plain JavaScript objects, plus a DOM-lite UI profile (elements, a small CSS subset, a 2D canvas) for people who want a page-shaped shell. Your OS is a folder of `.js` files copied to the card.

**Status: slice 3 of 7.** The contract ([docs/contract.md](docs/contract.md)) lists every kernel call and what it becomes in JavaScript, with the gaps. The runtime now lives here too: a clean import of `JS.ELF` from `gamemaker-os` ([IMPORT.md](IMPORT.md) has the source commit, every change and what slice 2 must split), with no API expansion beyond slice 3's `sandbox`, manifest and `fs.open`. How to write an app: [docs/apps.md](docs/apps.md). `gamemaker-os` keeps its own copy for now ([its guide](https://github.com/lewismoten/gamemaker-os/blob/develop/docs/js.md); the kernel's [author guide](../docs/os-authors.md) section 7 and [spike](../docs/js-spike.md)).

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
make -C js test      # host cases against tests/fixtures.json (log lines and framebuffer CRC-32 match the gamemaker-os build; only the heap-size figures may grow, within a budget), the profile interface tests (hook order, key routes, 50 start/stop cycles with a flat heap), and tests/app_run.py (manifests, capabilities, paths, handles, 50 app switches, AUTORUN)
make -C js size      # ELF bytes and each PT_LOAD against the 1 MiB ceiling and the 8 MiB image quota
make -C js repro     # two clean builds at different paths must match byte for byte
```

Current size (2026-11-13, toolchain 15.3.rel2, core profile, slice 3): `JS.ELF` 768,620 bytes (73.3% of 1 MiB, 279,956 spare); two PT_LOADs, R-X 759,312 bytes and RW- 2,340 file bytes with 3,960,916 in memory (the 3 MiB heap, a 256 KiB read buffer and QuickJS's data); loaded image 4.51 MiB of the 8 MiB quota (slice 2: 752,220 bytes). Run on the host, not yet on a board. Third-party notices: [third_party/README.md](third_party/README.md). Nothing here is flashed by the tests.

## Slice 1 needs (done)

1. A decision on the `JS.ELF` source move: copy from gamemaker-os with history or a clean import, and which repository builds `JS.ELF` afterward.
2. QuickJS-ng pinned (version and license text) and its build ported to this repository's Makefile and SDK.
3. Notices for QuickJS-ng, newlib pieces and the bundled pixel fonts, kept in NOTICE.md.
4. The fixtures that prove "existing behavior unchanged" (HELLOPI and the benchmark apps) and a host test harness that runs them against the kernel's host model.
5. ELF and PT_LOAD size reports in CI-free local scripts.

## Slice 2 (done)

The core no longer has a game dependency: `JS_GAME` is gone, replaced by `struct jsrt_profile` ([docs/profiles.md](docs/profiles.md)), a no-game core profile and a test profile.

## Slice 3 (done): versioning, packaging, lifecycle

An app folder carries an `APP.JSON` manifest (name, version, entry, the API it needs, the capabilities it asks for); the runtime refuses a malformed manifest or a newer API with a message in the log and on the picker's screen, gates every JS call on its capability, confines paths to the app's own folder (read-only) and save folder, counts every handle in one table that `jsrt_stop` releases and reports, and starts from `/OS/AUTORUN.TXT` with a fall back to the picker. `tools/app_check.py` validates an app on the host with the same rules. [docs/apps.md](docs/apps.md) is the author's guide. The decisions, all changeable: profile hooks read files only through `jsrt_fs_read`/`jsrt_fs_list` (the core's path policy); a garbage-collected playing element gets `end()` from its finalizer; an app with no manifest runs as a trusted legacy app (`jsrt_require_manifest` flips it); AUTORUN line 2 is `APPS/NAME` or `PICKER`; v1 apps are trusted maker apps.

Slice 4a (files and storage) needs: `fs.open` replaced by binary readers with seek on top of `jsrt_handle_*` (kind `JSRT_H_FILE`), `plat.h` gains write/stat/seek calls (the policy already has `app_path_check(..., write=1)` for saves and cache), a staged writer whose handle releases (aborts) at stop, and a decision on quotas versus `EDQUOT`. The kinds for audio, input, serial, e-ink and timers exist in `handle.h` for 4b to 4d; the capability checks are `need()` in `jsrt.c` (add one line per binding).

## Checking the contract

```sh
python3 js/tools/contract.py check   # schema, every syscall and ioctl covered, ABI minor current
python3 js/tools/contract.py md      # print the tables for docs/contract.md
```
