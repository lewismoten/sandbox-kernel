# sandbox-kernel SDK

Headers for programs that run on sandbox-kernel, MIT-licensed. The ABI is the
**draft 1.0** described in [docs/design.md](../docs/design.md#system-call-abi-draft-version-10);
it may still change before 1.0 is frozen.

- `include/sandbox/abi.h`: call numbers, error values, open flags, ioctl IDs and the
  fixed records, with size and offset asserts. Portable C11; no OS headers.
- `include/sandbox/syscall.h`: ARM-state ARM32 `svc #0` wrappers that return 0 or a
  negative `SBX_E*` value: `sbx_open`, `sbx_read`, `sbx_write`, `sbx_close`,
  `sbx_ioctl`, `sbx_log`, `sbx_fb_query`, `sbx_fb_present`, `sbx_exit`.

The kernel does not load programs yet (see [docs/m1.md](../docs/m1.md)). Build with
`-marm -mfloat-abi=soft` for ARM1176 targets and add `sdk/include` to the include path.
