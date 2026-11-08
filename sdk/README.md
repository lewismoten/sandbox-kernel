# sandbox-kernel SDK

Headers for programs that run on sandbox-kernel, MIT-licensed. The ABI is the
**draft 1.0** described in [docs/design.md](../docs/design.md#system-call-abi-draft-version-10);
it may still change before 1.0 is frozen.

- `include/sandbox/abi.h`: call numbers, error values, open flags, ioctl IDs and the
  fixed records, with size and offset asserts. Portable C11; no OS headers.
- `include/sandbox/syscall.h`: ARM-state ARM32 `svc #0` wrappers that return 0 or a
  negative `SBX_E*` value: `sbx_open`, `sbx_read`, `sbx_write`, `sbx_close`,
  `sbx_ioctl`, `sbx_log`, `sbx_fb_query`, `sbx_fb_present`, `sbx_exit`, and from slice 3
  `sbx_getdents` (directory entries of an open `/sd` folder), `sbx_health` (the
  heartbeat: within 10 s of the start, then at least every 2 s), `sbx_input_query`
  and `sbx_input_state_get` for `/dev/input/event0` (16-byte `sbx_input_event`
  records, Linux event codes; open it read-only with `SBX_O_NONBLOCK`), and from
  slice 4 (ABI 1.1; check `sbx_startup.minor`) `sbx_clock_ns` (monotonic
  nanoseconds, the input event clock) and `sbx_sleep_until` (wait without running
  until an absolute deadline; the heartbeat still has to come every 2 s).

- `sbx_fb_map` maps the `/dev/fb0` surface (`mmap`, call 6); `sbx_startup` is the
  read-only record whose address is in r0 at entry.
- ABI 1.3 adds `int32_t sbx_seek(uint64_t fd, int64_t offset, uint64_t *position)`
  (call 9). Check startup major 1 / minor >= 3; older kernels return `-SBX_ENOSYS`.
  Only regular `/sd` files and absolute offsets from 0 through file size (EOF)
  are supported. Success returns 0 and stores the new position. Negative or
  past-end offsets return `-SBX_EINVAL`; devices/directories `-SBX_ESPIPE`;
  invalid handles `-SBX_EBADF`. Failures leave both the file cursor and the
  caller's position output unchanged. See the full bounds/error contract in M1.
- ABI 1.4 adds metadata enumeration (`sbx_getdents_meta`, call18) and bounded
  `/sd/OS/CACHE/`-only create/write/fsync (`sbx_fsync`, call12) and copy-publish
  rename-over (`sbx_rename`, call19). Check startup minor >=4. This is not
  general filesystem write access or a power-loss journal. Close aborts unflushed
  bytes. See [the cache storage contract](../docs/cache-storage.md), including
  completed-sector durability and initially non-cross-linked FAT assumptions.
- `lib/crt0.S`, `lib/mem.c` and `lib/sandbox.ld`: entry (`int main(const sbx_startup *)`,
  whose return value is the exit status), `memset`/`memcpy`, and the link map (text
  at `0x08000000`, data on separate never-executable pages).

Build an ARM1176 program with the SDK only:

```
arm-none-eabi-gcc -mcpu=arm1176jzf-s -marm -mfloat-abi=soft -Os -ffreestanding \
  -nostdlib -Isdk/include -Tsdk/lib/sandbox.ld -Wl,-z,noexecstack \
  sdk/lib/crt0.S sdk/lib/mem.c program.c -lgcc -o PROGRAM.ELF
```

`programs/diag` is the example. Install a program on the card with
`scripts/usb_kernel.py <port> put PROGRAM.ELF` and start it with `run PROGRAM.ELF`;
`/OS/DIAG.ELF` starts by itself 10 s after boot. See [docs/m1.md](../docs/m1.md).
