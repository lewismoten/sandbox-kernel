# JS spike: JavaScript as the layer for making your own OS (2026-11-07)

The question was whether a JavaScript layer can let anyone build their own OS
shell on sandbox-kernel, written like a small web page, and still be fast
enough on the Pi Zero W, the slowest board in the plan.

The short answer is yes for shells, menus and app logic, if the drawing stays
native. The runtime, `JS.ELF`, is in
[gamemaker-os](https://github.com/lewismoten/gamemaker-os) (`src/js/`,
`docs/js.md`) because it hosts the Game-Maker play core. This page has the
engine choice, the measurements, what the kernel gained, and a plan.

## 1. Engine

| Engine | License | Language | ARM-mode code here (-Os, ARM1176) | Speed (host, same ES5 loop) | Notes |
| --- | --- | --- | --- | --- | --- |
| **QuickJS-ng 0.18.0** | MIT | ES2023 (`let`/`const`, arrows, classes, template strings, `Map`/`Set`, promises, modules) | 584 KB engine (quickjs 497, libunicode 56, libregexp 23, dtoa 8); `JS.ELF` 780 KB text with newlib and the play core | 0.116 ms per 1000-object update (-O2) | Chosen. Needs `malloc` and a small libc; a fixed heap is easy (`sys.c`). |
| MicroQuickJS (mquickjs, Bellard, commit 6d4d7eb) | MIT | ES5 subset, "stricter mode": no `let`, `const`, arrow functions, classes or template strings (each a SyntaxError when tried), arrays without holes | 127 KB (mquickjs 105, libm 13, dtoa 8) | 0.100 to 0.107 ms (-O2) | Same speed, a fifth of the code, a fixed buffer instead of `malloc`, bytecode in ROM. The fallback if code size or RAM ever bind. |
| Duktape 2.x | MIT | ES5.1 with some ES2015+ | not built (the project page states about 160 KB code and 64 KB RAM) | not measured | Slower than QuickJS on Bellard's published benchmarks. |
| JerryScript | Apache-2.0 | ES5.1 with much of ES2015+ | not built (the project page states under 200 KB ROM and 64 KB RAM) | not measured | Made for microcontrollers. Slower than QuickJS on Bellard's published benchmarks. |
| Elk | GPLv2 or commercial | a small subset | not built | not measured | Ruled out by its license (not MIT). |

There is no JIT on ARMv6 for any of them. They are all interpreters. The
ARMv6 numbers below are QuickJS-ng's. The deciding point was the language:
"easy for anyone" means the JavaScript people write today, and mquickjs rejects
it. QuickJS-ng costs about 450 KB more code for that, and code size is not a
constraint on a 512 MB board. The only limit it hit was the kernel's 512 KiB
PUTF install limit, which is now 1 MiB (below).

Sources: QuickJS-ng README and LICENSE (github.com/quickjs-ng/quickjs, tag
v0.18.0); MicroQuickJS README (github.com/bellard/mquickjs); Duktape
(duktape.org); JerryScript (jerryscript.net); Elk (github.com/cesanta/elk);
QuickJS benchmarks against Duktape, JerryScript and others
(bellard.org/quickjs/bench.html). The sizes and the loop timing in the table
were measured here; the other figures are as those pages state them.

## 2. The runtime (gamemaker-os `src/js/`, `docs/js.md`)

- QuickJS-ng is built in ARM state against newlib's libc and libm from the Arm
  GNU toolchain, with one fixed 3 MiB heap (`_sbrk` over a static array). There
  are no threads. Nothing in the image is Thumb (the kernel refuses Thumb state).
- **The API feels like the DOM (Lewis's direction).** `document.createElement('div'|'text'|'img'|'button'|'canvas'|'game')`,
  `appendChild`/`removeChild`/`insertBefore`, `element.style.*` (a CSS subset:
  px boxes, block and flex row/column, gap, padding, border, colors, font size,
  text align), `textContent`, `img.src` (a GIF or a game folder's title screen),
  `addEventListener` (keydown/keyup/click/focus/blur/ended, bubbling,
  `preventDefault`), `querySelector` with one simple selector, and focus moved by
  the arrow keys (Enter clicks). A `<canvas>` gives immediate drawing, and a
  `<game>` element plays a game with the native play core. It also has
  `innerHTML` for a small HTML subset, `requestAnimationFrame` on the kernel
  clock, `setTimeout`/`setInterval`, `console.log` to the USB log,
  `fs.readFile`/`readdir` (read-only), and `screen.setMode`.
- **Native owns the tree.** JS only sets properties. Layout, dirty rectangles
  (up to 16, merged), painting, image decoding and scaling, the game and its
  palette are C. Only damaged rectangles are presented.
- **HTML-like layout file.** `innerHTML` is about 40 lines of JS in
  `prelude.js`. HELLO's `INDEX.HTM` (9 lines) goes from read to laid-out
  elements inside its 86 ms start (below), so a tiny HTML subset costs next to
  nothing. A real HTML parser would not be worth it.
- **Hello OS:** `/OS/APPS/HELLO/MAIN.JS` and `INDEX.HTM`. Line 1 of
  `/OS/AUTORUN.TXT` says `JS.ELF` (or stays `GMOS.ELF` and you `RUN JS.ELF`),
  and line 2 names the app (`APPS/HELLO`).
  `usb_kernel.py <port> putapp apps/HELLO HELLO` installs the folder.
- Host tests (`make test` in gamemaker-os, `tests/test_js.py`) check the API
  probe (19 checks: layout, events, focus, timers, frames, canvas, files, GC of
  removed elements), the HELLO page at 960x540x16, a game played from JS and
  back, and that two runs on the virtual clock give the same picture.

## 3. What changed in the kernel (branch feature/js-spike)

- **PUTF up to 1 MiB** (the ELF loader's own limit; the stripped `JS.ELF` is
  789 KB), and `APPS/<DIR>/<NAME>` installs into `/OS/APPS/<DIR>/`
  (`usb_kernel.py putapp`).
- **ABI 1.2, screen modes:** ioctl `0x10002` sets width x height x {32, 16, 8}
  bpp. The firmware allocates the buffer and the GPU scales it to the HDMI mode
  at no CPU cost. Ioctl `0x10003` sets palette entries for 8 bpp. Present copies
  16 and 8 bpp bytes as they are. A mode must fit in the program's 1.2 MB
  surface. Every program still starts at 640x480x32, and the kernel goes back to
  it when the program ends.
- **Speed:** the ARM clock goes to its maximum at boot. The firmware left it at
  700 MHz and nothing raised it; it now runs at 1000 MHz. Branch prediction is
  on, and user pages are write-back (they were write-through). Together these
  made JS 1.6x faster and painting and presenting about 1.7x faster (table
  below). The full host suite passes, the board ran every benchmark and GMOS on
  it, and the 30 s trial confirmed it (generation 19).

## 4. Numbers (Pi Zero W, gamemaker-os `apps/BENCH`)

"Before" is generation 18 (700 MHz, no branch prediction, write-through user
pages); "after" is generation 19. A frame's time is JS, plus layout and paint,
plus present. Host is an Apple-silicon Mac with `jsrun` built at -O1, whose
present copies nothing.

| Measure | Host | Zero W before | Zero W after |
| --- | --- | --- | --- |
| Engine and browser-like globals ready | | | 18.8 ms, 90 KB heap |
| HELLO page started (mode switch, INDEX.HTM, six cards) | | | 85.8 ms; JS heap 100 KB, malloc high-water 225 KB |
| 1000-object update (JS) | 0.07 ms | 6.26 ms | **3.94 ms** |
| 640x480x32: key press (focus moves, two cards repaint, 94 K px) | 0.11 ms | 9.42 ms | **4.87 ms** (js 0.81, paint 1.57, present 2.49) |
| 640x480x32: all six card labels change | 0.03 ms | 3.45 ms | 2.13 ms |
| 640x480x32: full-screen repaint | 0.21 ms | 32.6 ms | 19.4 ms (paint 6.1, present 13.0) |
| 640x480x32: the same grid on a canvas, every frame | 0.43 ms | 42.5 ms | 26.1 ms (js 8.4, paint 9.4, present 8.3) |
| 640x480x16: key press / six labels / full / canvas | | 11.2 / 6.0 / 20.6 / 41.6 ms | **4.61 / 1.81 / 13.1 / 23.8 ms** |
| 960x540x16: key press / six labels / full / canvas | | 11.2 / 6.0 / 27.6 / 40.8 ms | **4.48 / 1.83 / 17.1 / 23.0 ms** |
| Game (Roll the Dice) from JS for 30 s: ticks in the play phase | 420 of 420 | 419 of 419 (320x200x8) | **420 of 420** (384x216x8) |
| Game: latest tick / longest step | | 16.2 / 3.1 ms | 7.6 / 1.6 ms |
| Game: JS frames alongside, each a 1000-object update | 60.0 a s | 55.4 a s | 59.5 a s (4.3 ms each) |
| Game: per-frame average (JS + the game's presents) | | 11.1 ms | 6.5 ms |

Mode switches, as the kernel measured them (the firmware call and clearing the
new buffer), after the speed changes: 640x480x32 3.1 to 4.4 ms, 640x480x16
2.3 ms, 960x540x16 3.8 ms, 384x216x8 0.57 ms, 320x200x8 0.79 ms (before).
Seen from JS, with the full repaint that follows, a switch is 4 to 7 ms.
Switching when a game starts and ends costs less than one frame.

What these show:

- **A key press costs about 5 ms** in any mode: under a third of a 60 Hz frame.
  Dirty rectangles make the mode's size nearly irrelevant for menus.
- **Full-screen repaints are the expensive case.** They take 13 to 19 ms (30 to
  50 a second at most), and half of that is the kernel's present: a per-pixel
  copy into uncached memory, 13 ms for 1.2 MB at 32 bpp and 6.6 ms at 16 bpp.
  16 bpp halves it. 960x540x16 costs about as much as 640x480x32 for a full
  repaint and the same as 640x480 for partial updates.
- **The DOM-style tree beats the immediate canvas** for this grid: 4.5 to 4.9
  ms against 23 to 26 ms, because only what changed is redrawn. The canvas
  version spends 8 ms in JS on about 40 draw calls a frame, so per-pixel or
  per-sprite work does not belong in JS.
- **The game keeps up exactly** at 18.2 Hz while JS runs a 4 ms task 60 times a
  second beside it. Its 8-bit mode presents 64 KB a tick and sets the palette
  only when it changes, so fades are palette writes, not pixel rewrites.
- Not measured: 1920x1080 output itself (it needs the new `config.txt` on the
  card; prepared in `.for-lewis/reference/pi-card/`), whether `scaling_kernel=8`
  applies to the firmware framebuffer, and HDMI blanking or flicker during a
  mode switch. Those need eyes on the screen. The 16 bpp colors (RGB565 order)
  and the palette's byte order also need a look on the screen. The kernel sends
  palette entries as R, G, B, A bytes.

## 5. Recommendation

- **JS is viable as the custom-OS layer on the Zero W** for shells, menus,
  settings, launchers and app logic, with the DOM-like API. Startup to a usable
  page is about 0.1 s, a key press costs about 5 ms, and the game core runs at
  full speed beside it.
- **Keep native:** layout, painting, image decoding and scaling, present, the
  Game-Maker play core, sound mixing (when it comes), file and USB I/O, and
  anything per pixel or per sprite at 60 Hz. JS calls them by setting
  properties.
- **Screen:** pages at 960x540x16 (exactly 2x of 1080p, crisp with nearest
  scaling) and games in an 8 bpp mode with their palette (384x216 is exactly 5x
  of 1080p with bars, or 320x240 for 2x on 640x480 output). Switch modes when a
  game starts and ends. Keep 32 bpp for the kernel's own screens and for
  programs that need it; it doubles the cost of a present.
- **Next kernel wins, in order:** a faster present (multi-word stores into the
  write-combining framebuffer, or no copy at all: a double-height virtual buffer
  and the firmware's virtual-offset tag to flip pages); then the 1080p
  `config.txt` on the card, checked by eye.

## 6. Milestone M2-JS (plan)

1. **Freeze the API surface:** `document.createElement` with the six tags,
   tree methods, `style` with the CSS subset as listed in gamemaker-os
   `docs/js.md`, `addEventListener` with the six event types, focus by arrows,
   `requestAnimationFrame`, timers, `console`, `fs.readFile`/`readdir`,
   `screen.setMode`/`gameLowres`, `<game>` (`src`, `play`, `stop`, `ended`).
   Version it (`sys.version`) and test it as a contract.
2. **Kernel:** zero-copy present (page flip); `exec` of another ELF from a
   program, so a JS shell can launch native apps; real input devices
   (gamepad, CEC) under the same events.
3. **Runtime:** a games page that matches GMOS in JS, as the proof that a shell
   can be rebuilt from a page; text in more fonts and sizes; `fetch` for local
   files; ES modules (`import`) between files in an app folder.
4. **Tooling:** `jsrun` as a "preview your OS on the computer" command, with a
   window, so makers see their page before copying it to the card.
5. **Decide later:** moving GMOS itself to 960x540x16; mquickjs only if code
   size or RAM becomes a constraint.
