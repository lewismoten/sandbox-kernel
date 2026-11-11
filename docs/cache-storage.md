# ABI 1.4: bounded cache-only SD storage

This is **not general SD write access**. Existing read/seek/getdents layouts and
all older minor-version applications remain compatible. USB PUTF keeps its
existing install allowlist. ABI 1.4 permits plain uppercase 8.3 names at the exact canonical path
`/sd/OS/CACHE/NAME`. ABI 1.7 also permits [per-game saves](saves-storage.md);
the two scopes share one writer. Physical writes and power cuts remain hardware-untested.

## Calls and SDK

- Open call 1: `SBX_O_WRONLY | SBX_O_CREAT | SBX_O_TRUNC` (`0x601`) stages an
  empty file in kernel RAM. Both create and truncate flags are mandatory. Only
  one writer exists; a second writer returns EBUSY. No append, read/write,
  arbitrary directories, path normalization, dot components or embedded NUL.
  Missing `/OS` and `/OS/CACHE` directories are created on commit.
- Write call 3: append to that staged buffer, bounded at **1 MiB total**. A
  capacity failure returns ENOSPC without changing the staged size. Normal SD
  read handles still reject write. The cache capability owns regular files in
  this expendable namespace, including files with FAT read-only attributes;
  do not keep user documents there. Directories and volume labels aren't files.
- `sbx_fsync(fd)`, call 12, uses the existing 24-byte close-shaped record. Commit
  data into free clusters, link both FAT copies, then publish one directory
  entry, then release the old chain. Every written sector is read back. The SD
  driver already waits for card programming completion with CMD13. Repeated
  fsync without intervening writes does no I/O. Close/reset **abort unflushed
  bytes**; they never implicitly commit. A partially written temp is not a cache.
- `sbx_getdents_meta`, call 18, uses the existing 48-byte directory I/O record.
  Its entries have a 24-byte header: `u16 length; u8 type,name_bytes; u32 size,
  modified,created,identity,reserved`, followed by the name and 8-byte padding.
  `modified` and `created` are raw little-endian FAT **date:time** words, not
  Unix timestamps. `identity` is the first cluster, **not a content hash**.
  Reserved is zero. Old call 11 still has its exact 8-byte header. Pending entries
  retain metadata even if the caller switches between the two record formats.
- `sbx_rename(from,from_bytes,to,to_bytes)`, call 19: 48-byte record, header plus
  four u64 fields. Both paths must be canonical cache-only names and different.
  Close/sync the writer first. Rename **copies** source data to an independent
  COW destination chain and publishes destination before deleting source. This
  deliberately avoids the cross-linked-cluster window of a two-entry FAT rename.
  Success leaves only the destination. A failure after publication may leave
  both names, but never intentionally shares their clusters. The destination
  may already be new when cleanup reports an error; this isn't full POSIX rename
  error atomicity. Readers must use only the committed destination.

Mutation is rejected while any CACHE or SAVES file/directory read handle is
open, including lowercase read-path aliases and USB GET/LS handles. Other asset handles are not remounted or
revoked. Kernel and SDK record layouts are independently asserted on host/ARM.

## Install invalidation and corrupt cache recovery

PUTF/USB installs still produce fixed 2026-01-01 FAT dates. Therefore names,
sizes and dates alone cannot detect same-size replacement. Before **any valid
USB fatput content mutation**, durably unpublish `/OS/CACHE/GAMES.IDX`. If that
unpublication fails, the install must stop. Cleanup after unpublication is
best-effort: a read-only attribute, oversized cache or malformed cache data chain
must not deny unrelated installs. Unvalidated chains are not reclaimed. A
malformed cache can likewise be replaced without freeing its unvalidated old
chain. These cases may leave orphan clusters for fsck, not live cross-links.

FSInfo locations are checked against the reserved area before mutation; forged
locations into a non-cache data file are rejected. Normal successful publication,
rename and removal maintain FSInfo counts and both FAT copies.

## Durability / safety boundary — important

The host failure model is an **initially structurally valid, non-cross-linked
FAT32 volume**, successful complete sector writes being durable, and failures
between whole-sector writes. Tests interrupt every write in initial creation and
rename, close/reopen/remount the media and verify complete old/new destination
(or missing during initial creation), independent remaining temp/destination
chains and unchanged boot/control/firmware files. Normal paths also pass fsck.

This is **not a journal, filesystem repair tool, or proof of physical SD power-cut
atomicity**. A cut can leak clusters, leave FAT copies different, or leave a
stale FSInfo count. Later writes fail closed on mirror mismatch and can require
`fsck.fat`/`fsck_msdos` on the host. The primary FAT reader and live discovery
remain available when their own metadata is intact. A malformed/torn cache
payload is rejected by GMOS's bounded checksum codec and rebuilt in RAM.

Arbitrary pre-existing directory aliases/cross-linked FAT metadata are outside
this guarantee: pathname allowlisting does not establish exclusive ownership of
all physical clusters. Do not use this capability to 'repair' an unclean/corrupt
filesystem. Arbitrary torn sectors or a card corrupting unrelated sectors can
exceed the failure model. Actual card-removal testing and latency qualification
are still required on the Pi before any unconditional power-loss safety claim.

Run `make test` (including `tests/test_cache.py`) and build with the ARM toolchain.
No hardware generation is implied by a host build ID.
