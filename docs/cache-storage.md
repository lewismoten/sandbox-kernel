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

Mutation is rejected while any cache file/directory read handle is open,
including lowercase read-path aliases. Other asset handles are not remounted or
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

## ABI 1.11: opt-in cooperative publication

Legacy `fsync` and copy/publish `rename` still complete synchronously; close/reset
still abort unsubmitted RAM bytes. Programs opt in with two new calls (check
startup major 1, minor >= 11):

- `sbx_cache_submit(fd, to, to_bytes)`, call **22**, 40-byte record: header,
  `u64 fd, to, to_bytes`. Consumes the staged writer handle on success and queues
  its immutable kernel RAM snapshot. No media access occurs in submission.
  `to=NULL, to_bytes=0` publishes to the writer name; otherwise `to` is a canonical
  writable path in the **same folder/scope**. Failure leaves the writer open.
- `sbx_cache_status(&record)`, call **23**, 32-byte record: header, `u64 state`,
  `i64 error` (outputs zero on entry). Polling performs no storage work. State is
  IDLE=0, PENDING=1, DONE=2 or ERROR=3; error is zero or a negative errno. There is
  one job; its result is retained until the next accepted submission. A new
  legacy writer does not clear that result. There is no cancellation operation.

Open/write as before, then submit directly to the committed destination, e.g.
open `GAMES.TMP`, stage bytes, submit to `/sd/OS/CACHE/GAMES.IDX`. **Do not fsync,
close or rename after successful submission**: the fd has been consumed. No TMP
file is created by this sequence. The COW chain is the temporary storage;
publication uses the same one-directory-entry switch as synchronous fsync, with
no second whole-file copy or source deletion. This also works when opening IDX
directly and submitting with a zero destination. Readers use only IDX.

One foreground kernel-loop pass resumes the worker once. It yields before sector
operations and every **1000 microseconds** during SD command/data/programming
waits and long free-cluster scans. The 128-word PIO transfer, short memory work
and context switch add overhead to that cooperative target; it is not a proven
physical wall-clock bound. IRQs stay enabled. A private 16 KiB privileged stack
preserves the existing FAT implementation's control flow and publication order;
HOST_TEST uses libc contexts instead of the ARM AAPCS stack switch. The worker
never calls `service_io`; USB and the program run after it yields. Timeouts
continue to count elapsed wall time, including time spent running the program.

While pending, a second cache/save writer and all other mutations return EBUSY.
**SD asset open/read/seek/directory enumeration also return EBUSY**; retry them after
completion. Existing asset handles remain valid. The controller may be midway
through a command, so unrelated SD access cannot interleave. Framebuffer, input,
clock, health, audio, e-ink and serial calls remain available. Trial confirmation
waits for the worker, and accepted jobs finish even if the submitting program
exits, faults or is stopped. New program launches/restarts wait for completion;
no user pointers or user pages are retained. Pending RAM is lost on power loss.

DONE means data readback, publication, old-chain release and FSInfo processing
have finished. ERROR can occur **after publication**, including readback/cleanup
failure; the destination may already be new. Re-read/validate it after completion
or retry from a retained application snapshot. The existing whole-sector power
failure model and limitations above are unchanged. Before publication, the old
destination remains intact; a cut at/after publication leaves old or complete new
bytes, with possible leaked clusters/mirror mismatch. This is not a journal.

### Host evidence and GMOS adoption (2026-11-15)

`tests/test_async_cache.py` exercises real FAT writes and the syscall dispatcher.
A full 1 MiB staged write with 10 ms injected per sector read **and** write spends
**164,390,000 us** in legacy fsync (including first directory creation). Submission
and status spend **0 simulated SD us**, and the largest cooperative worker slice
is **1000 us**. Native CPU/runtime overhead is excluded from these virtual-time
numbers. This stress model is not a measured PI-2 card latency. Completion reads
back the exact 1 MiB; failure/retry, second-writer EBUSY, invalid records/pointers,
save-folder scope, empty and nonempty saves, and accepted-job survival across
program reset are covered. Twenty-one write interruption points on replacement
remount to a complete old/new destination and preserve an unrelated asset. A
failure in the early data phase explicitly retains the old destination.

`tests/test_sd_commit.py` builds the real `drivers/sd.c` over simulated registers
and the same worker: a 50 ms card-programming delay takes **53,281 us** in one
synchronous call; async completion's maximum slice is **1000 us**, and the
one-second programming timeout's maximum slice is **1003 us**. The simulated
register accesses cost 1 us each; fixed PIO work and polling-check overhead count
in this test. The original cache/save write-cut suites continue to cover first
creation, rename, FAT mirror/FSInfo behavior and protected boot content. These
host models do not execute the ARM context switch or prove a physical bound.

GMOS can keep its idle-publication policy, stage chunks with health between them,
and replace fsync/close/rename with `sbx_cache_submit(fd, final_path, length)`.
After successful submission, retain its pending snapshot/revision until status
reports DONE; poll once per frame while servicing health/input/audio. ERROR
keeps the application's dirty snapshot for retry; EBUSY on SD reads or a writer
means defer that operation. Newer in-RAM discoveries must stay dirty if they
postdate the submitted snapshot. Clean Quit must wait through ordinary frames
and health calls for completion before exit. Gate this on startup >=1.11, keeping
the current synchronous idle fallback on older kernels. No gamemaker-os files
were changed here.

Board work still required: ARM build/review of the AAPCS stack switch and stack
headroom; real PI-2/Zero W frame/health/input/audio and worst-slice measurements
with slow/full/failing cards; controller timeout recovery; STOP/restart and trial
confirmation while pending; cache/save cold/warm restart validation; and physical
power interruption on spare media under the existing documented failure model.
No board, USB, flashing or ARM build is part of this host validation.

Validation on **Sunday, November 15, 2026 (EST, UTC−05:00)**:

- **03:25 EST** — read kernel storage/ABI guides and GMOS's list-stall evidence.
- **03:26–03:28 EST** — implemented opt-in submission/status, cooperative FAT/SD
  service and host slow-media/interruption tests; legacy cache tests passed.
- **03:29–03:34 EST** — full host validation identified hardcoded old ABI-version
  and unused-call fixtures; updated them for 1.11/next-unused call 24. Added the
  real-driver register model and verified retained asset handles/positions.
- **03:32–03:37 EST** — final `TOOLCHAIN=/nonexistent/host-only- make test` run:
  **450 tests, OK, 4 skips**, 244.233 s. ARM layout and two ARM example builds
  were intentionally disabled; the remaining skip needs mtools/mkfs.fat.
  Discovery preceded the final additional asset-handle regression; the final
  `test_async_cache.py` rerun separately passed **all 6 tests**, including it.
  Real-driver completion/timeout test, runtime/selector host syntax checks and
  `git diff --check` passed. Changes are uncommitted; GMOS was not edited.
