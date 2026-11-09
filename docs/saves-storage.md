# Per-game save folders: `/sd/OS/SAVES/<GAME>/`

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
attribute is not replaced (EEXIST-class refusal, `PUT NOT FILE`), and a file whose cluster
chain does not validate is an error, not something to overwrite. Saves are the player's data,
not expendable.

## Kernel pieces

`core/fatput.c`: `fatsave_put`, `fatsave_remove`, `fatsave_rename` (game + name, never a path).
`core/sys.c`: `save_path`/`write_path`. `core/assets.c`: `SAVES` counts as a guarded folder.
Tests: `make test`, `tests/test_saves.py`.
