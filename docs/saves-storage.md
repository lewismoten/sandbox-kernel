# ABI 1.7: per-game save folders, `/sd/OS/SAVES/<GAME>/`

ABI 1.7 introduced the `/OS/SAVES/<GAME>/` write scope. The current independent
kernel/SDK headers and startup record report **1.11**. This scope added no call
numbers or records. Older applications are unaffected; physical save writes
and power cuts remain hardware-untested.

Applications get a second, narrow write scope next to the cache (`docs/cache-storage.md`).
It is **not general SD write access**. The same calls apply (open `0x601`, write, `sbx_fsync`,
close, `sbx_rename`); only the accepted path grows.

## What is allowed

- Exact canonical path `/sd/OS/SAVES/<GAME>/<NAME>`. `GAME` is 1 to 8 of `A-Z 0-9 _ -`
  (no dot), one game's folder. `NAME` is a plain uppercase 8.3 name (`DICE.HIS`,
  `GAME0.SAV`, `SAVNAMES.DAT`). No `..`, extra levels, lowercase, empty parts or NUL.
- The kernel creates `/OS`, `/OS/SAVES` and `/OS/SAVES/<GAME>` on first commit.
- One writer at a time, shared with the cache writer (a second open is EBUSY). Same 1 MiB
  staging buffer. Close or reset without `sbx_fsync` discards the bytes.
- `sbx_rename` works inside one folder only: both paths in the same `<GAME>` folder.
  Renaming across games, or between a save folder and the cache, is EINVAL and writes nothing.
- Any open read handle on `/OS/CACHE` or `/OS/SAVES` (a file or a directory) blocks
  mutation with EBUSY, so a rewrite never moves a chain under a reader.

## Limits

At most **64 game folders** under `/OS/SAVES` and **32 files** in one game's folder. Creating a
new folder or file past a cap fails at `sbx_fsync` with `-122` (quota) and writes nothing;
existing games and files keep saving. The kernel's `KERNEL` log folder shares
these caps and uses one of the 64 folders. These count caps bound entries, not
total disk use; each staged file is limited to 1 MiB, and ENOSPC is still possible.

## Power-cut behavior

The write path is the cache's: data into free clusters, both FAT copies, then one directory
entry, then free the old chain; rename copies to an independent chain, publishes the
destination, then removes the source. An app that wants a crash-proof save writes
`NAME.TMP`, fsyncs, closes, then renames over `NAME.EXT`: after a cut the real name holds
the old file or the new one, never a torn mix (`tests/test_saves.py` cuts after every sector
write of a first create and of a rename and remounts). A cut can leave an orphan `.TMP`
and leaked clusters; fsck cleans them. This is not a journal and proves nothing about
physical SD behavior; the Pi has not been written.

Unlike the cache, a save folder is **strict**: a file with a read-only, hidden or system
attribute is not replaced (`PUT NOT FILE`). A save whose cluster chain does not validate
**can** be replaced by a new save, and the kernel can remove it: the directory entry is
replaced or unpublished and the unvalidated chain is never freed (it leaks until fsck), so a
bad card never blocks saving or frees clusters that may belong to something else.

## Kernel pieces

`core/fatput.c`: `fatsave_put`, `fatsave_remove`, `fatsave_rename` (game + name, never a path).
`core/sys.c`: `save_path`/`write_path`. `core/assets.c`: `SAVES` counts as a guarded folder.
Tests: `make test`, `tests/test_saves.py`.
