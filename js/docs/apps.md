# Writing an app (slices 3, 4a and 4b)

An app is a folder, `/OS/APPS/<NAME>/` on the card (`NAME` is 1 to 8 letters, digits, `_` or `-`; it is the app's id).
It holds an entry script (`MAIN.JS` unless the manifest says otherwise), any assets, and a manifest, `APP.JSON`:

```json
{
  "name": "Pocket Clock",
  "version": "1.0.2",
  "api": 1,
  "entry": "MAIN.JS",
  "capabilities": ["display", "input", "fs.read", "fs.write.saves"],
  "description": "A clock that remembers its format."
}
```

| Key | Meaning |
| --- | --- |
| `name` | Required. 1 to 32 printable ASCII characters. |
| `api` | Required. A whole number: the runtime API the app needs. The runtime refuses an app that needs a newer one than it has (`sandbox.version.api`; this runtime is API 1). |
| `capabilities` | The grants the app asks for (below). Leave it out for none. |
| `version` | Optional, `1`, `1.2` or `1.2.3`. Shown in the log. |
| `entry` | Optional, `NAME.JS` in the app folder. Default `MAIN.JS`. |
| `schema` | Optional, must be `1` if present. |
| `description` | Optional, 120 characters at most. |
| `x-anything` | Ignored, for your own notes. Any other unknown key is an error (it is usually a typo). |

The manifest is strict JSON of at most 2048 bytes, printable ASCII only. Check it before you copy the app to the card:

```sh
python3 js/tools/app_check.py MYAPP/            # one app: manifest, entry file, folder name
python3 js/tools/app_check.py --card /Volumes/SD  # every app, and the app AUTORUN.TXT names
```

`app_check.py` and the runtime apply the same rules (`src/app.c`; `tests/app_run.py` runs both on the same cases).

## Capabilities

A call an app has no grant for throws an `Error` that names the capability, for example
`fs.readFile: capability 'fs.read' is not granted to this app`. An app cannot grant itself anything; it only asks.

| Capability | What it opens today |
| --- | --- |
| `fs.read` | `fs.readFile`, `fs.readdir`, `fs.open`, `fs.stat`, `fs.readBytes` on the app's own folder (read-only) and its save folder |
| `fs.read.shared` | the same calls on the rest of the card outside `/sd/OS` (games, music). Add it to `fs.read`. |
| `fs.write.saves` | `fs.write` and `fs.createWriter` in the save folder `/sd/OS/SAVES/<id>/` |
| `fs.write.cache` | `fs.write` and `fs.createWriter` on the one file `/sd/OS/CACHE/<id>`, and reading that same file back (`fs.readFile`, `fs.open`, `fs.stat`); no other read comes with it |
| `display` | `screen.setMode`, `screen.gameLowres`, `screen.lease`, `screen.modes`, `document.createElement`, `requestAnimationFrame`, `<game>.play()` |
| `input` | the page's key events, and `input.open()` (raw queues). The host's F10 always works and is never delivered to an app. |
| `apps.launch` | `apps.list`, `apps.launch`, `apps.exit` (see Launching other apps) |
| `audio`, `eink`, `serial`, `power` | named now; the bindings come in slices 4c and 4d, and `sandbox.require` already refuses them |
| `log`, `time`, `memory`, `app.lifecycle` | always granted: `console`, timers, `sandbox.now`, `sandbox.sleep`, `Math.random`, `sys.stats`, `sys.exit`, `sys.launch` (`time` is in the base set, so a manifest does not ask for it) |

In JavaScript: `sandbox.version` (`{api, runtime}`), `sandbox.capabilities` (the granted names), `sandbox.has(name)`,
`sandbox.require(name)` (throws if not granted) and `sandbox.app` (`{id, name, version, legacy}`).

**An app with no `APP.JSON` still runs** as a *legacy app*: every capability, any path under `/sd`. This is the v1 default
(trusted maker apps; it keeps the HELLO, BENCH, CLOCK and PROBE fixtures byte-identical). Set `jsrt_require_manifest = 1`
(`jsrun --strict` on the host) to refuse such apps. Changeable default; ask Lewis before flipping it.

## Paths

An app sees its own folder read-only and its save folder read/write, and nothing else, unless `fs.read.shared` opens the
card outside `/sd/OS`. Paths start with `/sd/`; the card is case-blind, so case never matters. These are refused with a
reason: `..` or `.`, an empty name (`//`), a name ending in a dot or space (the card ignores them), the characters
`\ : * ? " < > | ~` and control bytes, a NUL, anything over 95 bytes, device paths (`/dev/...`), relative paths, another
app's folder, `/sd/OS` itself (including `AUTORUN.TXT`). The policy is `app_path_check` in `src/app.c`.

Profile hooks (`profile.h`) read files with `jsrt_fs_read` and `jsrt_fs_list`, which apply the same policy, not `plat_read`.

## Handles and teardown

Every open file (and every profile session) is an entry in the core's table, `handle.h`: at most 12 at once, each id
tagged with a generation that never repeats, so a closed or older handle never reaches a newer one (it throws
`EBADF: the file handle is closed or stale`). `jsrt_stop` runs the profile's `end` and `stop` hooks, then releases every
entry still open, logs `JS app left N handle(s) open: file 2` and returns N. A file handle the app drops without closing
is closed when the interpreter frees it; one it keeps is reported. 50 app switches leave the heap flat and zero handles open
(`tests/app_run.py`, `tests/files_run.py`). A staged writer is one entry too: left open at stop it is discarded, and reported.

A `<game>` element that is freed while playing gets `end()` from its finalizer (the page's screen mode returns on the
next turn). Timers and key listeners live inside the app's interpreter and vanish with it; they are not table entries. A screen lease (`display`), an input queue (`input`) and the sleep scheduler (`timer`, open only while a `sleep()` is waiting) are entries; stop releases them and reports what was left open.

## AUTORUN and recovery

`/OS/AUTORUN.TXT` line 1 is the kernel's (`JS.ELF`). Line 2 names the app: `APPS/NAME` or `PICKER`; with no line 2 the
default is `APPS/HELLO`. (Slice 3 narrowed line 2 from "a folder under /OS" to `APPS/NAME`, so an app's id is always known.)
If line 2 is bad, or the app is refused (bad or newer manifest, missing entry, a script that throws, or one stopped for time or memory: see Limits), the runtime logs why
and starts the always-embedded picker, which shows the reason on two rows ("Could not start APPS/NAME", then the reason).
The picker lists a folder that has a `MAIN.JS` or an `APP.JSON`.

## Limits (slice 5)

The runtime stops an app that runs too long or uses too much memory, and says why. These are the numbers (`src/jsrt.h`); the app
cannot change them.

| Limit | Value | What happens past it |
| --- | --- | --- |
| Script time, one loop turn | 500 ms | Everything the app runs in one turn counts together: input events, timers, animation frames, the sound refill, microtasks. |
| Script time, loading | 1,000 ms | The runtime's own start and the app's `MAIN.JS` run (also each start in a chain of launches). |
| JS heap | 1.5 MiB (1,536 KiB) | Counts every byte QuickJS allocates for the app (objects, strings, arrays, typed arrays and the compiled code). The board's malloc pool is 3 MiB and the page, canvases, file buffers and runtime take the rest. |
| Native stack of the script engine | 256 KiB | A runaway recursion throws `RangeError: Maximum call stack size exceeded`, which the app can catch. |
| Element nesting | 64 levels | `appendChild` and `insertBefore` throw `RangeError` past it (layout, painting and teardown recurse). |
| All canvases together | 640 x 480 pixels (1.17 MiB) | A canvas that would pass it, or one over 960 x 540, throws `RangeError`. |

The kernel's heartbeat is 2 seconds, so the longest the loop can go without calling it is the 1,000 ms start (a chain of starts calls it
before each) or a 500 ms turn plus the native work in it (drawing, a file read). A script slice that runs out of time is stopped by an
interrupt QuickJS polls about every 10,000 operations, so a `while (true) {}`, an endless microtask chain, a regular expression that backtracks
forever and a long `sort` are all caught; a long native call that never returns to the interpreter (one huge `fs.readBytes`, at most 1 MiB)
is bounded by its own limits instead.

**What the user sees.** The stop is not an exception the app can catch (a `try`/`catch` or `finally` around the loop never runs). The runtime
logs `JS app stopped: a script ran too long (limit 500 ms)` (or `it ran out of memory (limit 1536 KiB)`), closes everything the app held (audio,
the e-ink panel, serial, power holds: the log says `app left N handle(s) open` if it was not closed), and returns:

- to the app that launched it, which starts again with `sandbox.launchError = {id, reason}` (the reason above); or
- to the AUTORUN app when nobody launched it and it is another app; or
- to the picker, which shows `Could not start APPS/NAME` and the reason.

The exit status the log shows is 124 for time and 137 for memory (what `timeout(1)` and a killed process use). It is never a heartbeat fault:
the runtime is still in control, so the kernel sees an ordinary app switch.

**Memory is a hard stop, even if the app catches the error.** The heap limit is soft by 128 KiB: the allocation that passes 1.5 MiB succeeds
and marks the app to be stopped at the next interrupt poll (within about 10,000 operations), so the stop never happens in the middle of
QuickJS's own compiler (QuickJS-ng 0.18.0 crashes when an allocation fails inside `js_parse_class` and a few more places, found by the sweep
in `tests/alloc_run.py`). One request that alone passes 1.5 MiB + 128 KiB (`new ArrayBuffer(64 << 20)`) fails cleanly with an out-of-memory
error and stops the app too. Allocation failures in the runtime's own C bindings (elements, canvases, handles, the file writer's growing buffer,
the page root) become `OutOfMemory` errors in JS, never a crash. A 64-bit host build counts a JS value as 16 bytes and the board as 8, so a
script that fills 1 MB on the host fills about half of that on the board.

Plan for it: stream a long sample or a big file in pieces (`fs.open` + `read`, `audio.open({onneed})`) instead of holding it; do heavy work in
slices of a few milliseconds across frames (`setTimeout`/`requestAnimationFrame`), as the 500 ms is a guard against hangs, not a budget (aim for 2
to 4 ms a frame); and keep a long loop out of `MAIN.JS`. Tests: `tests/limits_run.py` (runaway loops, microtask chains, regular expressions,
heap fills, recursion, element depth), `tests/alloc_run.py`, `tests/soak_run.py`, `tests/fuzz_run.py`.

## Launching other apps (slice 7, gap G5)

The kernel has no exec call, so the runtime switches apps itself: the one app that runs lives inside JS.ELF, and a switch stops it
(every handle released, as at any stop) and starts the next in a fresh realm. Add `"apps.launch"` to `capabilities`.

```js
for (const a of apps.list()) console.log(a.id, a.name, a.version, a.ok ? 'ok' : a.reason);
// {id: 'HELLO', name: 'Hello', version: '1.0', api: 1, capabilities: ['log', ...], ok: true, reason: ''}
apps.launch('NOTES', { args: { open: 'todo' } });   // returns at once; this app is stopped when the turn ends
// in NOTES:  sandbox.launchArgs -> { open: 'todo' }  (undefined when there are none; JSON, 200 bytes at most)
apps.exit(3);                                        // in NOTES: back to the app that launched it
// back in the caller (a fresh start):  sandbox.returned -> { id: 'NOTES', code: 3 }
```

- `apps.list()` reads each `/OS/APPS/<id>/APP.JSON` with the same validator as a launch and returns only that metadata
  (`ok: false` and a `reason` for a bad manifest, a newer API or a missing entry). It gives no access to another app's files, and
  a folder with neither `APP.JSON` nor `MAIN.JS` is not listed. At most 32 apps.
- `apps.launch(id, {args})` throws `RangeError` when 4 apps are already waiting (the launch stack is 4 deep). The caller is
  not resumed: it starts again from the top when something returns to it, with its own `launchArgs` and with `sandbox.returned`
  (who exited and with what code), so keep state you need in a save file or in the arguments.
- `sandbox.launchDepth` is how many apps wait for this one (0 when nothing launched it), so an app can offer "back" only when there is somewhere to go.
- `apps.exit(code)` returns to the app that launched this one. With nobody on the stack it goes to the AUTORUN app (unless that is
  this app) or the picker. `sys.exit` and F10 still go to the picker and clear the stack.
- A launch that does not start (bad manifest, newer API, missing entry, a script that throws) returns to the caller with
  `sandbox.launchError = {id, reason}`; `apps.on('launcherror', fn)` calls `fn` once, right after the start, if there is one.
  An unstartable app never leaves a blank screen.
- Give back the e-ink panel, sound and anything else you hold before launching when you can; the runtime releases what is left
  and logs `app left N handle(s) open`.

TINYOS (`examples/TINYOS`) is the worked example: its home lists its own pages and then every other app in `/OS/APPS`, and `HELLO`
returns to it on Esc. Tests: `tests/launch_run.py` and `tests/os_run.py`.

## Files and storage (slice 4a)

```js
// Read a binary file in pieces.
const f = fs.open('/sd/OS/APPS/MYAPP/LEVEL1.DAT');   // needs fs.read
f.size; f.position;                                  // bytes
f.seek(16);                                          // 0 to size (the end is allowed); RangeError outside
const head = f.read(64);                             // Uint8Array, fewer bytes at the end, empty at the end
const buf = new Uint8Array(4096);
const n = f.readInto(buf, 0, 4096);                  // into your own buffer; returns the count
const line = f.text(80);                             // UTF-8 text
f.close();                                           // or let it go; stop closes it and reports it
const all = fs.readBytes('/sd/OS/APPS/MYAPP/LEVEL1.DAT');  // the whole file (at most 1 MiB)

fs.stat(path);                                       // {name, type: 'file' | 'dir', size, modified, created}
fs.readdir(path);                                    // names, as before
fs.readdir(path, { meta: true });                    // [{name, type, size, modified, created}] with long names

// Save: stage in memory, publish at commit (needs fs.write.saves).
fs.write('/sd/OS/SAVES/MYAPP/SLOT1.SAV', bytesOrString);        // one call
const w = fs.createWriter('/sd/OS/SAVES/MYAPP/SLOT1.SAV');
w.write(header); w.write(body);                      // returns the staged size
try { w.commit(); } catch (e) { if (e.code === 'EBUSY') { /* close readers in the save folder, retry */ } else w.abort(); }
// w.commit() and w.close() PUBLISH the staged bytes. w.discard() and w.abort() THROW THEM AWAY. Never close() in a finally.
```

How it works, and the limits that come from the kernel:

- **Reads** are real kernel handles. Reads of a long-name file work with its long name. A read returns at most 256 KiB;
  loop for more. `modified` and `created` are the card's FAT date and time words, kept as opaque numbers.
- **Writes** are whole files. The kernel can only write a complete file (create or replace) to a canonical name under
  `/sd/OS/SAVES/<id>/` (the app's folder name) or the one file `/sd/OS/CACHE/<id>`. So a writer keeps its bytes in the
  app's memory and `commit()` does the kernel's single write (open, write, flush, close). Nothing touches the card before
  that. `abort()`, a dropped writer, or the app stopping discards the bytes and leaves the card as it was. The kernel replaces
  a save by writing new data, then one directory entry, so a power cut leaves the old file or the new one.
- **commit, close, discard, abort.** `commit()` and `close()` are the same call: both publish what is staged and close the
  writer (`close` is kept because authors expect it to finish the file). To give up, call `discard()` (or its older name
  `abort()`): the card is left as it was, and a second call does nothing. So do not close a writer in a `finally` after an
  error; that would save a half-built file. Put `discard()` in the `catch` instead.
- **An empty writer** commits a zero-length file (the kernel's open with create-and-truncate already marks the file to be
  written, so its flush publishes an empty one). `fs.write(path, '')` does the same: it creates the file or empties it.
- **Reading the cache back.** An app with `fs.write.cache` may read its own `/sd/OS/CACHE/<id>` (a read capability is implied
  for that one path). It can not read another app's cache file, and the cache capability alone opens no other path.
- **Big folders.** `fs.readdir` feeds the kernel's heartbeat as it goes, so a folder of thousands of files cannot trip the
  2 s watchdog inside one call. The 1.5 MiB heap is the real limit: `{ meta: true }` builds an object per file, so list a
  folder with thousands of entries without `meta`.
- **Design note: no temp file and rename.** A commit is the kernel's single whole-file write (new data first, then one
  directory entry), which already leaves the old file or the new one after a power cut. The cache's temp-file-then-rename
  (`sbx_rename`) would add a copy and a window with two names, so the runtime does not use it for saves.
- **Names** are 8.3: 1 to 8 letters, digits, `_` or `-`, optionally a dot and 1 to 3 more. Lowercase is turned into
  uppercase. One folder deep, no subfolders, no `.` or `..`.
- **Quota.** An app may keep 256 KiB in its save folder (all its files together; the file being replaced does not count
  against itself), and a staged file may not exceed it: `EDQUOT`. The kernel's own limit of 1 MiB per file (`ENOSPC`) and
  its 64 games and 32 files per game (`EDQUOT`) still apply. The default is `jsrt_write_quota` in `src/jsfs.c`.
- **One writer** at a time (the kernel has one): a second `createWriter` is `EBUSY` until the first commits or aborts.
- **Readers block commits.** While any file handle is open in the save folder (or cache), a commit fails with `EBUSY`; close
  it and call `commit()` again (a failed commit keeps the writer open).
- **Errors** are `Error`s with `code` and a message that starts with it: `ENOENT`, `EACCES` (no capability, or a path
  outside the grant for a read), `EROFS` (a write outside the save or cache path, including the app's own folder),
  `EINVAL` and `ENAMETOOLONG` (name rules), `EBUSY`, `EDQUOT`, `ENOSPC`, `EBADF` (closed handle), `EMFILE`/RangeError (too
  many handles, 12 per app). `fs.readFile` and `fs.readdir` keep their slice 1 messages.
- No append, no in-place update, no delete, no folders: the kernel has none (contract gaps G2 and G11 to G14).

Status: all of this is host-tested (`make -C js test`); none of it has run on a board yet.

## Display, time and input (slice 4b)

```js
// Modes. screen.setMode(w, h, 32|16) is the page's, as before; 8 bpp is only for a lease (or a game).
screen.modes();                 // [{width, height, depth, pitch, bytes}]: common sizes that pass the kernel's rule
screen.supports(960, 540, 16);  // the rule itself: width 16..1920 and a multiple of 4, height 16..1200, <= 1,228,800 bytes

// A lease: the app draws pixels itself. The page (DOM-lite) stops drawing until release().
const l = screen.lease({ width: 320, height: 200, depth: 8 });   // no argument: the page's current mode
l.pixels;                       // Uint8Array (8 bpp), Uint16Array (RGB565) or Uint32Array (0x00RRGGBB), l.pitch bytes a row
l.setPalette([0, 0, 0, 255, 255, 255], 0);   // 8 bpp: r,g,b bytes from entry `first`; the screen keeps the top 6 bits of each
l.getPalette();                 // Uint8Array(768): what the screen shows (255 stays 255; 130 comes back 130 widened from 32)
l.pixels.fill(1); l.present();  // or present(x, y, w, h); RangeError outside the surface
l.release();                    // the page's mode returns and it repaints itself; l.pixels is detached (length 0)

// Time.
sandbox.now();                  // whole microseconds on the kernel's clock: exact, monotonic, since kernel start
await sandbox.sleep(250);       // also the global sleep(ms); resolves in order of due time, at least `ms` later
sandbox.seed; sandbox.seedRandom(7);   // Math.random's seed; the second makes a run reproducible

// Input: raw events beside the page's own.
const q = input.open();         // needs `input`; four queues at most
q.on('key', (e) => e.code, e.value, e.repeat, e.time);   // Linux key code; value 1 down, 0 up, 2 repeat; e.time in microseconds
q.on('attach', () => {}); q.on('detach', () => {});
q.on('reset', (e) => e.reason, e.released);              // 'dropped' or 'detached', and the key codes that were let go
q.pressed(103); q.down; q.attached; q.close();
```

- **Ownership.** A lease and the page never draw at once. While a lease is open, `screen.lease()` again and `screen.setMode()` fail with `EBUSY: ...`, `<game>.play()` too, and a lease fails with `EBUSY` while a `<game>` is playing. The page keeps its elements and timers and `requestAnimationFrame` still runs (draw there), but nothing of it reaches the screen. `release()` (or stop, or the collector freeing a dropped lease) puts the page's mode back and repaints all of it. A lease in the page's own mode starts with the page's pixels on it; a lease in another mode starts cleared. A view kept after `release()` has length 0, and every other call on the released lease throws `EBADF`.
- **Palette.** An 8 bpp lease starts with a gray ramp so something shows; the kernel's palette stays set until the next one.
- **Sleep.** The kernel's heartbeat limit is 2 s and `sleep_until` does not service it, so the loop never sleeps more than 100 ms at once and calls the health call between sleeps: a `sleep(60000)` is hundreds of small kernel sleeps. With a keyboard attached the loop looks at input every 10 ms anyway. Up to 64 sleeps may wait at once (`EMFILE`); `ms` is 0 to 2,147,483,647 (`RangeError`). `sleep(0)` yields: it resolves on the next turn, after already queued microtasks.
- **Random.** `Math.random` is a 128-bit xorshift started from the best the kernel offers, which is only timer jitter: the clock at app start, the startup record and 64 back-to-back clock reads, with each key's timing mixed in as keys come. That is a handful of unpredictable bits, fine for variety, not for secrets. The kernel has no random source (contract gap G7). `sandbox.seedRandom(n)` makes the sequence reproducible and stops the mixing.
- **Raw input.** Raw events arrive before the page's, for every key but F10. The page's `keydown` and `keyup` are unchanged. `SYN_DROPPED` (the kernel lost events) and a device that goes away both mean no key can be trusted: the runtime releases every held key, fires `reset` with their codes and sends the page a `keyup` for each. A key still held then shows nothing until it is pressed again (repeating keys resume). At most 128 events are handled a turn, so a flood cannot starve timers and frames; `sys.stats().inputMaxPerStep` shows the largest turn.
- **Detach.** The kernel gives no notice: a read error means the device went away (`detach`, then `reset`), and the runtime tries to reopen it every 100 ms (`attach`). With no device the loop wakes every 100 ms instead of 10. Event times are stamped when the runtime reads them, up to 10 ms after the key.

Status: all of this is host-tested (`make -C js test`, `tests/dev_run.py`); none of it has run on a board yet.

## Audio (slice 4c)

A short square-wave melody, played with the built-in tones (needs `audio`):

```js
const a = audio.open();                                 // stereo, 48 kHz: the kernel's only format
a.volume = 0.6;                                         // 0 to 1, in software (the kernel has no volume)
a.melody([[262, 200], [330, 200], [392, 200], [523, 400], [0, 100], [392, 200], [523, 600]]);   // [hz, ms], hz 0 is a rest
sleep(2500).then(() => a.close());
```

The same device for your own samples. `onneed` is called when the ring has room, so the app does not schedule anything:

```js
const rate = 24000, pcm = new Int16Array(rate * 2);     // two seconds, mono
for (let i = 0; i < pcm.length; i++) pcm[i] = (i % 60 < 30 ? 6000 : -6000);   // 400 Hz square
let at = 0;
const a = audio.open({ rate, channels: 1, onneed(frames) {
  at += a.write(pcm.subarray(at, at + Math.min(frames, 512)));     // the frames taken; 0 is "full", not an error
  if (at >= pcm.length) a.close();
} });
a.stats();   // {rate, capacity, queued, played, underruns, overruns, overrunFrames, appRate, channels, pendingTones, writes, refused, framesIn, refills, gaps}
```

- **One writer.** `audio.open({rate, channels, onneed, targetMs})` takes the kernel's `/dev/audio` (`EBUSY` if the app already holds it or another program does, `ENODEV` on a kernel older than 1.8). `close()` (or stop, which reports it as `audio 1`) drops what is queued and frees the device for the next app. The format is fixed at 48,000 stereo frames a second: `channels: 1` is doubled, and a `rate` that divides 48000 (8000, 12000, 16000, 24000, 48000) is repeated sample by sample. Other rates are `EINVAL`.
- **Writes never block and may be partial.** `write(Int16Array)` (interleaved for two channels) queues what the 16,384-frame ring (341 ms) has room for, in whole frames, and returns how many frames it took: possibly fewer than you gave, possibly 0. Keep the rest and offer it later (`onneed` tells you when). The array must hold whole frames (`EINVAL`). A blanked screen drops audio silently (the call still succeeds), so the pump stops while the screen is dark (see Power below).
- **The refill pump.** With `onneed` set, the runtime looks at the ring every 20 ms and keeps it about `targetMs` (default 200, 20 to 300) full: it calls `onneed(frames)` with the room it sees, up to 4,096 kernel frames a turn, again while the app keeps writing, and not again until the next look when the app wrote nothing. The loop never sleeps more than 100 ms, so a ring that is a fifth full at worst still plays on through any single sleep. An exception in `onneed` is logged and the pump carries on.
- **Tones.** `tone(hz, ms, volume = 1)` queues a square-wave beep (20 to 20,000 Hz; `hz` 0 is a rest; 32 queued at most, `EMFILE`); `melody(notes)` queues several. The runtime synthesizes them into the ring through the same pump (the kernel's own mixer tones are not an app call), so while tones are pending, `write()` returns 0 (tones own the ring) and `onneed` is not called. `stop()` forgets tones not yet written; what is already in the ring (up to `targetMs`) still plays.
- **Counters.** `stats()` has the kernel's: `underruns` (one for each millisecond the ring ran dry, counted only after the first write), `queued`, `played`, `capacity`, in kernel frames; and the runtime's: `refused` (writes that took less than offered), `refills`, `gaps` (looks that found the ring empty), `writes`, `framesIn`. `overruns` stays 0 because the runtime asks for the room first and queues only that.
- **Volume** is software on your writes and tones, `volume` 0 to 1 and `muted`; a muted app keeps its timing (frames still go through, as zeros).

Status: all of this is host-tested against a model of the kernel's ring (`make -C js test`, `tests/audio_run.py`); none of it has run on a board, and the HDMI audio path itself is hardware-untested. Kernel wishes: G18 and G19. While the screen is blank the pump does not synthesize, does not call `onneed` and leaves queued tones queued, unless the app holds `noBlank` (below); it carries on at the next look after the wake.

## E-ink, serial and power (slice 4d)

An animated page on the 250x122 e-ink panel (needs `eink`). A page is 3,904 bytes: 32 bytes a row, bit 7 the leftmost pixel, 1 is black. The panel limits refreshes (1 s apart, 30 a minute); the runtime paces for you, so draw as often as you like and only the newest page goes out:

```js
const p = eink.open();                 // null when there is no panel (eink.available is false)
const c = p.canvas();                  // clear, setPixel, fillRect, drawBitmap, flush; shapes are clipped to the page
let n = 0;
setInterval(() => { c.clear(); c.fillRect(n++ % 240, 50, 10, 10); c.flush(); }, 100);   // sent about once a second
p.update(3, 5, 10, 3, new Uint8Array([0xff, 0xc0, 0, 0, 0x80, 0x40]));   // one region (x, y, w, h, bits); out of bounds is EINVAL
p.setTitle('MY OS', { build: 'V0.1' }); p.page(new Uint8Array(3904)); p.stats(); p.release();
```

- `page(bytes)`, `update(x, y, w, h, bits, {send})`, `flush()`, `canvas().flush()` and `setTitle(...)` return `true` when the update went to the panel now and `false` when it was queued. A queued update is replaced by a newer one (`stats().replaced`) and sent by the loop when the kernel's limits allow (`min interval`, `maxPerMinute`, from `p.info`); a kernel `EAGAIN` is retried 250 ms later, never in a spin. `p.bits` is the page the runtime keeps, a `Uint8Array(3904)`.
- One owner (`EBUSY`). `release()` and stop clear the panel back to the kernel's screen when an update may go at that moment (the kernel clears at the end of the program anyway); `close()` leaves the page up. A page still queued at stop is dropped.

Serial (needs `serial`) is a byte channel to the host, nonblocking, with partial reads and writes:

```js
const s = serial.open({ mode: 'rw' });      // 'r', 'w' or 'rw'; one owner (EBUSY); ENODEV without the link
s.on('data', (bytes) => s.write(bytes));    // echo: bytes is a Uint8Array of at most 256 bytes
const buf = new Uint8Array(64), n = s.read(buf);   // n bytes now, 0 when nothing waits
const took = s.write('hello\n');            // a string is sent as UTF-8; took may be less than its length
s.stats();                                   // the kernel's counters plus appRead, appWritten, events
```

The 'data' event is fed by a poll every 20 ms with at most 1,024 bytes a turn, so a flood cannot starve the loop: the kernel's 256-byte buffer keeps what it can and counts the rest in `rxDropped`. With no listener nothing is read for you. A `write` returns what was taken (up to 4,096 a call; the real kernel takes everything and counts overflow in `txDropped`, gap G21).

Power (needs `power`): keep the screen lit while you play sound or read, and learn when it blanks:

```js
const h = power.hold('music');                       // noBlank by default; {noSlowdown: true} for the clock too; 16 holds at most
power.on('blank', () => console.log('screen off')); power.on('wake', () => console.log('back'));
power.asleep;      // {blank, slowdown}: what the kernel has put to sleep now
power.screenDark;  // true while the screen is blank and this app holds no noBlank
h.release();       // twice is fine; stop gives back every hold and logs it
```

The kernel drops audio while the screen is blank, so the audio pump stops synthesizing then (see Audio). An app that wants sound through a blank screen takes a `noBlank` hold, which also keeps the screen lit. The blank state is polled at most every 100 ms, only while a listener or the audio pump needs it. A kernel older than 1.12 reports "lit". `power.setTimeouts` is not bound (gap G20).

Status: host-tested against models of the panel, the link and the power table (`tests/devices_run.py`); none of it has run on a board, and the e-ink and serial paths are hardware-untested in the kernel. Kernel wishes: G20 and G21.
