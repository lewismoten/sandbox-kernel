# Import record

Slice 1 is a clean import (no history) of the JS runtime from the `gamemaker-os` repository
(<https://github.com/lewismoten/gamemaker-os>, MIT). This was the default chosen on 2026-11-13;
it can change (a history-preserving move, or this repository becoming the only source).
`gamemaker-os` keeps its own copy and is unchanged by this slice.

- Source: checkout of `gamemaker-os` at `44fe7e016e97effc84c31135fdc38907909d3966`
  (branch `feature/gmos-menu-sounds-90s-1112`); the imported files last changed at
  `5d863fb792a7780ae06404cc04da8733038a3449` (2026-11-10) and `third_party/quickjs-ng` at
  `ad42afa51f95200e2763b700a691d37e3b7b243d`. Its SDK pin was sandbox-kernel `32fd0d2`; this
  build uses this repository's own `sdk/` (ABI 1.12), which is newer.

| Here | From | Change |
| --- | --- | --- |
| `src/{jsrt,dom}.c, jsrt.h, dom.h, main.c, plat.h, plat_sbx.c, sys.c, prelude.js, picker.js` | `src/js/` | `jsrt.c`: everything that needs the play core is behind `JS_GAME` (default 0, see below); `dom.c`: the font include path |
| `src/gif.c, gif.h` | `src/` | `fli.h` include replaced by a no-op `gp_decode_checkpoint()` (the runtime never installed the hook) |
| `src/font.h, font_data.c`, `fonts/font.txt` | `src/`, `tools/` | none |
| `third_party/quickjs-ng/` | `third_party/quickjs-ng/` | none; checked against the upstream tarball |
| `tools/embed_js.py` | `tools/` | none |
| `tests/host/jsrun.c` | `tests/host/` | two include paths |
| `apps/{HELLO,BENCH,CLOCK,PROBE}` | `apps/`, `tests/js/PROBE` | none |

`tests/fixtures.json` was recorded from a `jsrun` built from that source commit (see
`tools/make_fixtures.py`); `tests/run.py --ref <that jsrun>` compares live instead.

## Slice 2: the profile split

The `JS_GAME` blocks in `jsrt.c` are replaced by `struct jsrt_profile` (`src/profile.h`); `src/profile_core.c` is the
default no-game profile and `tests/host/profile_test.c` the test one. The old play core code is not here: see
[docs/profiles.md](docs/profiles.md) for where each piece goes in gamemaker-os. Differences a fixture could see: none
(the fixtures still match); `play()`'s TypeError no longer mentions `JS_GAME`. `jsrun` gained `--cycles N`
(start and stop N times and print the live heap blocks).

With the core profile `<game>` is an inert element: created and sized, `play()` throws a TypeError that says the build
has no game profile, `press()` and `stop()` do nothing, `game.ticks` reads 0 and the `sys.stats()` game counters stay 0.
A game folder as an `<img src>` fails with the existing "cannot show" log line; `.gif` sources work (`gif.c`). HELLO's
Games button and BENCH's last section report an empty card; fixtures use a card with no `/GAMES`.

## Slice 3: apps, capabilities, handles

New: `src/app.{h,c}` (manifest, capabilities, path policy, AUTORUN line), `src/handle.{h,c}` (handle table), `jsrun` flags
`--autorun`, `--strict`, `--alt`, and `jsrt_stop` returning an int. `jsrt.c` gains `need()` / `path_ok()` checks on the existing
natives, `fs.open`, `sandbox.*`, the manifest start and the leak report; `main.c` starts through `jsapp_autorun`;
`picker.js` shows why an app did not start and lists a folder with an `APP.JSON` too. Differences a fixture could see: the two
heap-size figures in log lines grow by under 8 KiB (the `sandbox` global and new natives), so `tests/run.py` compares them
within that budget and everything else (log text, framebuffer CRCs, exit codes) exactly. `tests/fixtures.json` is unchanged.
AUTORUN line 2 is now `APPS/NAME` or `PICKER` (it was any folder under `/OS`).
