# Sandbox JS changelog

## 0.1.3 (2026-11-13)

Save safety, from the review of 0.1.2. Host-tested only; not run on a board. No ABI change.

- After any failed `write()` (`EDQUOT`, `ENOSPC`, out of memory, wrong type) the writer is spoiled: later writes, `commit()` and `close()` throw `EIO` and discard it, so the old file stays. A `close()` in a `finally` after a failed write used to publish the part staged before it (a 600-byte save became a 100-byte one); a host test now covers that case.
- `docs/apps.md` no longer promises that a power cut leaves the old file or the new one. The kernel's docs say save writes are hardware-untested against power cuts, so the page now says that and suggests two save slots with a generation number and checksum.
- Error texts: `ENOSPC` means the card or the kernel's staging space is full, and 1 MiB is a per-write limit; `EFBIG` comes only from the kernel's rename, which the runtime never calls. `apps.md` lists `EISDIR`, `EFBIG` and `EIO`.
- `screen.gameLowres` changes nothing when its size check fails (it used to switch the flag first).
- Correction to 0.1.2's note: the kernel does offer apps a rename (`sbx_rename`); saves skip temp-and-rename by choice (see `apps.md`, design note), not for lack of one.

## 0.1.2 (2026-11-13)

Second pass over the adversarial review. Host-tested only; not run on a board. No ABI change.

- A cache write no longer counts other apps' files in the shared cache folder against its quota (each app has one cache file, limited when staging).
- A read that fails partway keeps `position` in step with the kernel's file offset.
- `screen.gameLowres` size check uses a 64-bit product (65536 x 65536 wrapped to 0 and passed).
- Kernel errors -21 and -27 reach the app as `EISDIR` and `EFBIG` instead of a generic `EIO`.
- Left on purpose: `close()` still commits (use `discard()`); saves still have no temp-and-rename (needs a rename the kernel does not offer apps).

## 0.1.1 (2026-11-13)

Follow-ups from the adversarial review (`docs/apps.md`, Files and storage). Host-tested only; not run on a board.

- `fs.readdir` (plain and `{ meta: true }`) feeds the kernel heartbeat per directory batch and every 64 names, so a huge folder cannot trip the 2 s heartbeat. Host test with a 5,000-entry folder (`--beats`).
- Committing a writer with nothing staged writes a zero-length file (tested; the kernel's open marks the file dirty, so the flush publishes it).
- `writer.discard()` is the explicit abort. `close()` stays the same as `commit()`; the docs now say so plainly.
- An app with `fs.write.cache` can read back its own `/sd/OS/CACHE/<id>` (`fs.readFile`, `fs.open`, `fs.stat`), and nothing else.
- Design note kept: saves do not use a temp file and rename.

## 0.1.0 (2026-11-13)

First numbered release. Built and tested on the host only; **not yet run on a board.**

- Slice 0: the contract of kernel calls and their JavaScript shapes (`docs/contract.md`, `contract.json`).
- Slice 1: JS.ELF built from this folder with QuickJS-ng 0.18.0 and the toolchain's newlib; reproducible build (`make repro`), size report (`make size`).
- Slice 2: a generic core with a profile interface; the Game-Maker profile stays outside (`docs/profiles.md`).
- Slice 3: `sandbox` object, `APP.JSON` manifests, capabilities, path policy, handle accounting, app teardown.
- Slices 4a to 4d: files and storage; display, time and input; audio; e-ink, serial and power.
- Slice 5: execution limits (500 ms script slice, 1.5 MiB heap, stack and nesting caps), allocation-failure sweep, soak and fuzz tests.
- Slice 6: the version baked into JS.ELF (`sandbox.version.runtime`), `make drift` (ABI fingerprint), `LICENSES/` and a generated `NOTICES.TXT`, and `make bundle` (a card folder).
- Launcher: the recovery picker, `OS/AUTORUN.TXT` line 2 names the app to start, and apps can launch apps (`apps.list`, `apps.launch`, `apps.exit`).
- TINYOS: an example home screen (apps, files, settings that survive a reboot, startup chime, e-ink status), plus the HELLO and CLOCK apps.

Kernel: ABI 1.12 (kernel commit e67d02f).
