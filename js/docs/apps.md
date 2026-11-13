# Writing an app (slice 3)

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
| `fs.read` | `fs.readFile`, `fs.readdir`, `fs.open` on the app's own folder (read-only) and its save folder |
| `fs.read.shared` | the same calls on the rest of the card outside `/sd/OS` (games, music). Add it to `fs.read`. |
| `fs.write.saves` | the save folder `/sd/OS/SAVES/<id>/` (writing arrives with slice 4a) |
| `fs.write.cache` | the one file `/sd/OS/CACHE/<id>` (slice 4a) |
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
(`tests/app_run.py`). `fs.open(path)` returns `{size, text(), close()}` holding the file's bytes: a stand-in that slice 4a
replaces with binary reads and seeking, so the handle rules (not the shape) are what it proves.

A `<game>` element that is freed while playing gets `end()` from its finalizer (the page's screen mode returns on the
next turn). Timers and key listeners live inside the app's interpreter and vanish with it; they are not table entries yet.

## AUTORUN and recovery

`/OS/AUTORUN.TXT` line 1 is the kernel's (`JS.ELF`). Line 2 names the app: `APPS/NAME` or `PICKER`; with no line 2 the
default is `APPS/HELLO`. (Slice 3 narrowed line 2 from "a folder under /OS" to `APPS/NAME`, so an app's id is always known.)
If line 2 is bad, or the app is refused (bad or newer manifest, missing entry, a script that throws), the runtime logs why
and starts the always-embedded picker, which shows the reason on two rows ("Could not start APPS/NAME", then the reason).
The picker lists a folder that has a `MAIN.JS` or an `APP.JSON`.

Status: all of this is host-tested (`make -C js test`); none of it has run on a board yet.
