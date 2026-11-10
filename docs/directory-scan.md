# Directory lookup sector reuse

`core/assets.c` keeps a 512-byte directory sector only for one `find()` or
`asset_readdir()` invocation. Sixteen raw FAT directory entries share a sector;
looking for an absolute path component no longer rereads that sector for each
entry. The buffer is separate from the global file and FAT buffers. A successful
read publishes the LBA; an error leaves it invalid. Existing directory validation,
chain bounds and whole-file chain validation remain in force.

There is **no cross-call or persistent directory cache**, no new ABI, and no SD
writer change. An edit between asset calls is visible on the next call, as before.
A normal readdir returning one short entry still does fresh I/O on its next call;
this intentionally avoids an unproven installer/media invalidation protocol.

`tests/test_directory_scan.py` compiles the actual asset reader against fake SD
media. It covers the read budget for a late entry in a fragmented directory,
interleaved handles plus file reads, metadata edits between calls, and retry after
a failed read. The late-entry test fails on the base reader with 139 reads against
a budget of 16, then passes with sector reuse.

The separate GMOS profiling tool can reproduce synthetic SD-read counts:

```sh
python3 /path/to/gamemaker-os/tools/profile_fat.py . /private/output
make test
make TOOLCHAIN=/path/to/arm-none-eabi-
```

In task t_04029321, the 135-folder synthetic fragmented FAT32 microprobe's
per-folder directory-open/list plus GAM-open phase fell from 21,272 to 2,332
`sd_read` calls. It does not read the GAM payload, is not a real SD layout, and is
not a Raspberry Pi time measurement. Five native x86_64/stdio samples gave median
11.781552 ms before and 2.853447 ms after for that phase; real-card speedup is
unverified. The actual production scanner still performs its normal discovery,
CRC duplicate checks and library enrichment.

This optimization itself does not persist `GAMES.IDX`. ABI 1.4 subsequently
added metadata, bounded cache writes and USB-install invalidation; see
[cache-storage.md](cache-storage.md). Cache decoding belongs to the OS. Board
FAT transaction/failure-recovery and real-card speedup remain hardware-untested;
this invocation-local read buffer does not establish media durability.
