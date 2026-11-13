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

## What the game profile needs (slice 2's split)

`JS_GAME=1` is not buildable here: it needs gamemaker-os's `game.cpp` (play core), `session.c`,
`dosmenu.c`, `sound.c`, `thumb.c`, `fli.c`, `text.c`, `font8_data.c`, `third_party/gmcore`. With it
off (the default):

- `<game>` still exists in the DOM (`createElement('game')`), draws nothing, and `play()` throws
  a TypeError that says the build has no game profile; `press()` and `stop()` do nothing.
- A game folder as an `<img src>` (its title screen) fails with the existing "cannot show" log line;
  `.gif` sources work (`gif.c`).
- `screen.gameLowres()` validates as before; `game.ticks` reads 0; the `sys.stats()` game counters stay 0.
- HELLO's Games button and BENCH's last section report an empty card; fixtures use a card with no `/GAMES`
  for that reason, so they cover the generic runtime, not game play.
- The Game-Maker-only code sits in `jsrt.c` as `#if JS_GAME` blocks: the session, `find_gam`, `path_read`,
  `dir_read`, `picture_read`, `draw_game`, `play`/`stop`/`press`, the key route, the per-frame game step and wake,
  and `jsrt_stop`'s release. Slice 2 should turn those into a profile interface (init, step, wake, draw,
  key, stop) instead of preprocessor blocks.
