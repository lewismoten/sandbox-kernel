# Writing an app (slices 3 and 4a)

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
| `fs.write.cache` | `fs.write` and `fs.createWriter` on the one file `/sd/OS/CACHE/<id>` |
| `display` | `screen.setMode`, `screen.gameLowres`, `document.createElement`, `requestAnimationFrame`, `<game>.play()` |
| `input` | key events (the host's F10 always works) |
| `audio`, `eink`, `serial`, `power` | named now; the bindings come in slices 4c and 4d, and `sandbox.require` already refuses them |
| `log`, `time`, `memory`, `app.lifecycle` | always granted: `console`, timers, `sys.stats`, `sys.exit`, `sys.launch` |

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
next turn). Timers and key listeners live inside the app's interpreter and vanish with it; they are not table entries yet.

## AUTORUN and recovery

`/OS/AUTORUN.TXT` line 1 is the kernel's (`JS.ELF`). Line 2 names the app: `APPS/NAME` or `PICKER`; with no line 2 the
default is `APPS/HELLO`. (Slice 3 narrowed line 2 from "a folder under /OS" to `APPS/NAME`, so an app's id is always known.)
If line 2 is bad, or the app is refused (bad or newer manifest, missing entry, a script that throws), the runtime logs why
and starts the always-embedded picker, which shows the reason on two rows ("Could not start APPS/NAME", then the reason).
The picker lists a folder that has a `MAIN.JS` or an `APP.JSON`.

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
```

How it works, and the limits that come from the kernel:

- **Reads** are real kernel handles. Reads of a long-name file work with its long name. A read returns at most 256 KiB;
  loop for more. `modified` and `created` are the card's FAT date and time words, kept as opaque numbers.
- **Writes** are whole files. The kernel can only write a complete file (create or replace) to a canonical name under
  `/sd/OS/SAVES/<id>/` (the app's folder name) or the one file `/sd/OS/CACHE/<id>`. So a writer keeps its bytes in the
  app's memory and `commit()` does the kernel's single write (open, write, flush, close). Nothing touches the card before
  that. `abort()`, a dropped writer, or the app stopping discards the bytes and leaves the card as it was. The kernel replaces
  a save by writing new data, then one directory entry, so a power cut leaves the old file or the new one.
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
