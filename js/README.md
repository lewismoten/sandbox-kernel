# Sandbox JS

Working name. A layer on top of sandbox-kernel so you can write your own OS for the Pi Zero W **in JavaScript alone**: no C, no cross compiler, no kernel source.

You write apps in JavaScript. Sandbox JS is one ELF (QuickJS-ng inside) that the kernel boots like any program; it turns the kernel's small system-call surface (files, display, input, audio, e-ink, serial, power) into plain JavaScript objects, plus a DOM-lite UI profile (elements, a small CSS subset, a 2D canvas) for people who want a page-shaped shell. Your OS is a folder of `.js` files copied to the card.

**Status: slice 4d of 7.** The contract ([docs/contract.md](docs/contract.md)) lists every kernel call and what it becomes in JavaScript, with the gaps. The runtime now lives here too: a clean import of `JS.ELF` from `gamemaker-os` ([IMPORT.md](IMPORT.md) has the source commit, every change and what slice 2 must split), with slice 3's `sandbox` and manifest, slice 4a's files (handles with binary reads and seek, `fs.stat`, listings with types and sizes, a staged writer with a quota) and slice 4b's display, time and input (a surface lease for direct pixels, modes and palettes, a microsecond clock, `sleep`, a seeded `Math.random`, raw input queues). How to write an app: [docs/apps.md](docs/apps.md). A complete example OS and the step-by-step to boot your own: [docs/make-your-own-os.md](docs/make-your-own-os.md) ([`examples/TINYOS/`](examples/TINYOS/)). `gamemaker-os` keeps its own copy for now ([its guide](https://github.com/lewismoten/gamemaker-os/blob/develop/docs/js.md); the kernel's [author guide](../docs/os-authors.md) section 7 and [spike](../docs/js-spike.md)).

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
| 4a, 4b, 4c, 4d (done) | Files and storage; display, time and input; audio; e-ink, serial and power. |
| 5 | Execution limits and hardening (script budgets, allocation failure, soak). |
| 6 | Release and maker path: drift checks against the kernel ABI, prebuilt artifacts, a JS-only example OS another author can boot. |
| 7 | Optional adoption by gamemaker-os. |

Design and estimates: the owner's research note `2026-11-10-js-layer-design.md` (kept with Lewis's research folder, not in this repository).

## Build and test

```sh
make -C js           # build/JS.ELF (JS_PROFILE=file.c picks another profile) (stripped), build/JS.full.elf; needs the Arm GNU Toolchain
make -C js host      # build/host/jsrun, the same runtime over stdio
make -C js test      # host cases against tests/fixtures.json (log lines and framebuffer CRC-32 match the gamemaker-os build; only the heap-size figures may grow, within a budget), the profile interface tests (hook order, key routes, 50 start/stop cycles with a flat heap), tests/app_run.py (manifests, capabilities, paths, handles, 50 app switches, AUTORUN), tests/files_run.py, tests/dev_run.py, tests/audio_run.py and tests/devices_run.py
make -C js size      # ELF bytes and each PT_LOAD against the 1 MiB ceiling and the 8 MiB image quota
make -C js repro     # two clean builds at different paths must match byte for byte
```

Current size (2026-11-13, toolchain 15.3.rel2, core profile, slice 4d): `JS.ELF` 825,972 bytes (78.8% of 1 MiB, 222,604 spare; slice 4c: 801,396; slice 4b: 793,204; slice 4a: 780,916; slice 3: 768,620); two PT_LOADs, R-X 816,064 bytes and RW- 2,348 file bytes with 3,967,204 in memory (the 3 MiB heap, a 256 KiB read buffer and QuickJS's data); loaded image 4.57 MiB of the 8 MiB quota. Run on the host, not yet on a board. Third-party notices: [third_party/README.md](third_party/README.md). Nothing here is flashed by the tests.

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

## Slice 7 (done): apps launching apps

Capability `apps.launch`: `apps.list()`, `apps.launch(id, {args})`, `apps.exit(code)` (gap G5, no kernel change). `jsapp_step` in `src/jsrt.c` owns a launch stack (4 deep) and the target's `sandbox.launchArgs`; a failed start returns to the caller with `sandbox.launchError`. `tests/launch_run.py` covers it (jsrun `--blocks` prints live heap blocks and open handles after every app stop); TINYOS's home lists and launches the apps on the card ([docs/apps.md](docs/apps.md)).

## Slice 4a (done): files and storage

`fs.open` is a real handle (`JSRT_H_FILE`) over the kernel's open, read, seek and close; `fs.stat` and `fs.readdir(path, {meta:true})` come from the folder listing with long names; `fs.write` and `fs.createWriter` stage in the app's memory and publish with the kernel's one whole-file write, under a quota (256 KiB per app by default). The host model in `tests/host/jsrun.c` follows the kernel's save rules (8.3 names, one writer, readers block commits, 1 MiB, 64 games and 32 files). The file API is documented in [docs/apps.md](docs/apps.md); the new kernel wishes are gaps G11 to G14 in the contract. Slice 4b needs: display, time and input bindings on the same pattern (a `need()` line per binding in `jsrt.c`, a class in its own `js*.c`, `JSRT_H_INPUT`/`JSRT_H_TIMER` entries); none of the file work blocks it.

## Checking the contract

```sh
python3 js/tools/contract.py check   # schema, every syscall and ioctl covered, ABI minor current
python3 js/tools/contract.py md      # print the tables for docs/contract.md
```

## Slice 4b (done): display, time and input

`src/jsdev.c` holds the bindings (the same pattern as 4a). `screen.lease()` hands the mapped surface to the app as a typed array, exclusive with the DOM-lite page (`JSRT_H_DISPLAY`); `screen.modes()` and `screen.supports()` give the kernel's mode rule; an 8 bpp lease has `setPalette`/`getPalette`. `sandbox.now()` is exact microseconds, `sandbox.sleep(ms)` a promise the loop wakes in chunks of at most 100 ms (`JSRT_H_TIMER`), and `Math.random` starts from a seed built from what the kernel offers (timer jitter only; `sandbox.seed`, `sandbox.seedRandom`). `input.open()` gives raw queues (`JSRT_H_INPUT`) with key (including repeat), attach, detach and reset events; recovery after `SYN_DROPPED` releases every held key. The page's own key events are unchanged. [docs/apps.md](docs/apps.md) has the API and the rules. New kernel wishes: G15 to G17 (the contract also notes what the runtime does about G1, G3 and G7). Slice 4c (audio) needs: a handle kind already exists (`JSRT_H_AUDIO`), a `need()` for `audio`, a refill pump in `jsrt_step` bounded like the input one (the 100 ms chunk is also the refill budget) and a host model of the 48 kHz ring in `tests/host/jsrun.c`; see the report for the details.

## Slice 4c (done): audio

`src/jsaudio.c` binds `/dev/audio` (ABI 1.8): `audio.open({rate, channels, onneed, targetMs})` is the one writer (`JSRT_H_AUDIO`, released at stop), `write(Int16Array)` takes what the ring has room for and returns the frames taken, a pump in `jsrt_step` (every 20 ms, bounded per turn) calls `onneed(frames)` to keep the ring about 200 ms full, `tone`/`melody` synthesize square waves in C, `stats()` has the kernel's counters and the runtime's own, `volume`/`muted` are software. The host model of the 48 kHz ring in `tests/host/jsrun.c` (virtual clock, `--audio-dump`, `--no-audio`, `--audio-busy`) drives `tests/audio_run.py`. The ceiling for the heap growth in `tests/run.py` went from 24 KiB to 40 KiB (the audio prelude; it measured 32.8 KiB). New kernel wishes: G18 (volume, mute, flush, tones for apps) and G19 (silence frames and blank drops in the counters). Slice 4d (e-ink, serial, power) needs: `JSRT_H_EINK`/`JSRT_H_SERIAL` entries and a `need()` each in the same pattern; host models of the 250x122 page with its refresh rate limit (EAGAIN means too soon), the serial link's partial reads and writes, and `power_hold`'s token table; `power.asleep` events from the loop (audio is dropped while dark, so the player should stop synthesizing); and a decision on how to share the blank/wake state with the pump.

## Slice 4d (done): e-ink, serial and power

`src/jseink.c`, `src/jsserial.c` and `src/jspower.c` (the same pattern). `eink.open()` is the one owner of the panel (`JSRT_H_EINK`): a 3,904-byte page from a `Uint8Array`, a region `update(x, y, w, h, bits)`, or a small canvas; the runtime paces by the kernel's limits and keeps only the newest queued page (the pump sends it later; a kernel `EAGAIN` is retried every 250 ms). `serial.open({mode})` (`JSRT_H_SERIAL`) has partial `read`/`write` and a 'data' event from a bounded poll (256-byte events, 1 KiB a turn). `power.hold(reason)` takes a token in the kernel's 16-entry table (given back at stop, which logs it); `power.asleep`, `power.screenDark` and 'blank'/'wake' events come from the ABI 1.12 query, polled at most every 100 ms. Decision: while the screen is blank the audio pump stops synthesizing and does not call `onneed` unless the app holds `noBlank`, as the kernel drops audio then. Host models in `tests/host/jsrun.c` (panel with limits, link with a receive and transmit buffer, power table with a blank schedule), tests in `tests/devices_run.py`. The ceiling for the heap growth in `tests/run.py` went from 40 KiB to 80 KiB (it measured 68.2 KiB; the three preludes add about 36 KiB). Not bound: `power.setTimeouts` (the kernel keeps timeouts after an app switch, G20). New kernel wishes: G20 (EAGAIN gives no retry time, the e-ink clear at an app boundary may be refused, power timeouts persist) and G21 (serial write takes everything and drops silently).
