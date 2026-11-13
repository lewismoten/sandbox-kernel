# Profiles: how an optional feature plugs into the core

The core (`src/jsrt.c`, `dom.c`, the prelude, `gif.c`) is the generic runtime: files, timers, frames, the element tree,
input, the screen. It knows nothing about any game engine. Anything optional reaches it through one small C struct,
`struct jsrt_profile` in [`src/profile.h`](../src/profile.h), chosen at build time. The test profile
(`tests/host/profile_test.c`) is a working example of every hook.

## The default: the core profile

`src/profile_core.c` defines `jsrt_profile` with every hook NULL. Decision for the no-game build: **`<game>` is a documented
inert element.** `document.createElement('game')` works, it can be sized and appended and has `src`, `playing` (always
false) and `ticks` (always 0); `play()` throws `TypeError: play(): this build has no game profile`; `stop()` and `press()`
do nothing; it never draws. A game folder as an `<img src>` logs the usual "cannot show" line; `.gif` sources work (the
core reads those itself). `screen.gameLowres()` validates exactly as before (the 320x200 picture size is the core's
`JSRT_PIC_W`/`JSRT_PIC_H`). Apps can feature-detect by catching that TypeError.

## The interface

Every hook may be NULL. All run on the one interpreter thread and must not call `jsrt_*` back.

| Hook | The core calls it | Notes |
| --- | --- | --- |
| `init()` | in `jsrt_start`, after the core's state is reset, before the JS context exists | once per app start, even one that then fails |
| `stop()` | in `jsrt_stop`, after `end()` if an element was playing, before the context is freed | runs exactly once per `init`, so a failed start is cleaned up too |
| `play(dir, &handle, err, cap)` | `game.play()` with the element's `src` | 0 on success (`handle` is what `draw` gets; NULL is fine), or -1 with a message in `err` that the core throws as a RangeError |
| `end()` | the element stopped: `game.stop()`, Esc or the end (`JSRT_END`), or the app stopped | release what `play` took; the core then restores the page's screen mode and fires `ended` |
| `key(code, value)` | a Linux key code (value 1 down, 0 up, 2 repeat) while the element has focus, or `game.press()` | return `JSRT_END` to finish the element, else 0; F10 never reaches it (the host owns it) |
| `step(now, &direct)` | each turn of the loop while playing, after JS frame callbacks, before microtasks | return `JSRT_DRAW` (and in the direct 8 bpp mode fill `direct`) and/or `JSRT_END` |
| `wake(now)` | when the core works out how long to sleep | the next time the element needs a turn in ns, or 0; the core takes the earliest of this, timers and frames |
| `draw(handle, fb, stride)` | the page paints the element's rectangle (a 32 bpp page, or `screen.gameLowres(false)`) | stride in pixels |
| `ticks()` | `game.ticks` | |
| `picture(src, px, pal)` | an `<img src>` that is not a `.gif` | fill `JSRT_PIC_W` x `JSRT_PIC_H` indices and a 768-byte 6-bit palette; 0 or -1. Not called while an element is playing |

Order, per app: `init`; zero or more of (`play`; then `key`, `step`, `wake`, `draw` as they come; `end`); `stop`. The
core owns that order, the screen mode (`lowres_on`, `set_mode`), the direct blit and palette change check, the stats
counters (`sys.stats()` `game*` fields; `jsrt_stats` is in `jsrt.h` for a profile to add to), focus and events.

The direct mode: with `screen.gameLowres(true)` (the default) the core puts the screen in 8 bpp, and when `step`
returns `JSRT_DRAW` with `direct.px` and `direct.pal` set, copies the 320x200 picture centered into the surface,
sends the palette only when it changed, and presents. In `gameLowres(false)` it only repaints the element and `draw`
paints it.

## Building one in

`jsrt_profile` is a `const struct jsrt_profile *const` defined by exactly one file in the link. Make variables:

```sh
make -C js JS_PROFILE=/path/to/profile_gm.c JS_PROFILE_FLAGS='-I/path/to/gamemaker-os/src' JS_PROFILE_LIBS='/path/to/game.o ...'
make -C js host JS_PROFILE=... JS_PROFILE_LIBS=...   # the same for jsrun, built with the host compiler
```

`JS_PROFILE` replaces `src/profile_core.c` (one file, compiled with the core's warning flags and `-Isrc`);
`JS_PROFILE_FLAGS` adds include paths and defines to every core compile; `JS_PROFILE_LIBS` is extra objects or
archives for the link (build them for the target yourself). The size budget and the reproducibility check run on
whatever you build, so a profile that does not fit under 1 MiB fails `make size`.

## For gamemaker-os: plugging in the Game-Maker profile

No change in this repository is needed, and nothing in gamemaker-os changed in this slice. In gamemaker-os, replace the
`#if JS_GAME` blocks of its `src/js/jsrt.c` by a file `src/js/profile_gm.c` that defines `jsrt_profile`, and take the
core from here. The old blocks map like this (the names are the ones in the pre-slice-2 `jsrt.c`):

| Old `#if JS_GAME` code | Profile hook |
| --- | --- |
| `static struct gs_session session; memset(&session, 0, ...)` in `jsrt_start` | the session lives in `profile_gm.c`; `init()` clears it |
| `path_read`, `dir_read`, `find_gam`, and the `gs_start` call in `js_play` (the "no .GAM in %s" and "cannot play %s" errors, `session.say = say`, the `JS game %s/%s` log line) | `play()`: list the folder for a `.GAM`, `gs_start(&session, dir_read, dir, gam, plat_clock_ns())`, write the error text into `err`, log with `jsrt_log()`, return `&session` as the handle |
| `session.total_ticks` in `P_TICKS` | `ticks()` |
| `gs_event(&session, 1, code, value) & GS_EXIT` in `key_event` and `js_press` | `key()`: return `JSRT_END` when it says `GS_EXIT` |
| the per-turn block (`gs_wake`, late and play-time stats, `gs_step`, `session.t0` reset on entering `GS_PLAY`, `GS_DRAW`, `GS_EXIT`) | `step()`: keep `static uint64_t last` there, update `jsrt_stats.game_*`, return `JSRT_DRAW` for `GS_DRAW` (with `direct.px = session.px`, `direct.pal = session.pal`) and `JSRT_END` for `GS_EXIT`. The 320x200 copy, `shown_pal` and `plat_present` are now the core's |
| `gs_wake` at the end of `jsrt_step` | `wake()` |
| `gs_draw` as `draw_game` | `draw()` |
| `picture_read`, `find_gam` and `gm_title_picture` in `load_picture` | `picture()`; the "a game is playing, so refuse" test is the core's |
| `gp_release_all()` in `stop_game` and in `jsrt_stop` | `end()` |
| `snd_end(&session.sound)` in `jsrt_stop` (only when playing) | `stop()`: end the sound if a play is still open (track that with a flag set in `play()` and cleared in `end()`; note the core calls `end()` before `stop()`, so a sound ended in `end()` is already done) |

The play core, `dosmenu.c`, `sound.c`, `thumb.c`, `fli.c`, `text.c`, `font8_data.c` and `third_party/gmcore` stay
in gamemaker-os; build them for the target into objects and pass them as `JS_PROFILE_LIBS` (and their include paths
in `JS_PROFILE_FLAGS`), or keep them in a profile file that `#include`s them. `GP_W`/`GP_H` are `JSRT_PIC_W`/`JSRT_PIC_H`.
Keep your fixtures (a game-card case) in gamemaker-os; this repository's fixtures are the no-game path and must stay
byte-identical.

Slice 3 changes, none of which a fixture sees: hooks read files with `jsrt_fs_read` / `jsrt_fs_list` (profile.h), which apply
the running app's path policy (see [apps.md](apps.md)), instead of `plat_read`; a session a hook allocates can be a core
handle (`jsrt_handle_open(JSRT_H_GAME, release, user)`, `handle.h`): `end()` closes it, and one left open at stop is released
and reported by the core (the test profile does exactly that, and its `stop` logs `jsrt_handles_open()`); `play` is refused
for a game folder the app may not read, and `picture` is not called for a source it may not read; an element freed while
playing gets `end()` from its finalizer. `jsrt_stop` now returns the number of leaked handles.
