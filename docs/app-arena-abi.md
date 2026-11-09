# ABI 1.6 app arena (Slice C)

This is an opt-in kernel interface, not an audio device or general allocator.
The 40-byte `k_startup` / `sbx_startup` types and offsets remain unchanged.
Runtime publishes a separate 48-byte extension with an application epoch at
40, major 1/minor 6 and prefix.size 48. Health continues using the boot
generation at offset 8. New consumers gate major, minor and size before reading
the epoch. The SDK wrapper returns ENOSYS without issuing a call on legacy
startup, and leaves caller outputs unchanged on every error (including skewed
kernels returning ENOSYS). Disable the optional feature; never fall back to
anonymous mmap. The historical GMOS SDK pin is not refreshed by this change.

Call 21 accepts the 48-byte `sbx_app_arena_rec` / `k_app_arena`:
header16, u64 owner_generation16, requested_bytes24, address32, actual_bytes40.
Record VA is 8-aligned; register convention is unchanged. Extensions through
4096 bytes in 8-byte steps must have zero unknown tails, preserved on success.
Both outputs must be zero on entry. Full record copy-in/header/tail/output-zero
and writable-range checks precede active/stopping, epoch and length checks.
Malformed records win over operation semantics; inactive/STOP wins over stale
epoch, which wins over an invalid requested size. Errors preserve user records,
prior live bytes and mappings (ordinary syscall count still advances).

Only the actual RUNNING supervisor owner can acquire, with its exact nonzero
u64 epoch and requested minimum in 1..2195456 inclusive. First success charges
all 536 pages from the existing 16 MiB pool at `[0x30000000,0x30218000)`, RW/XN.
The adjacent guards remain absent. No desired VA, PA, executable protection,
resize, free, FD, mmap reinterpretation or quota expansion is provided. Same
owner repeats return the fixed address/full size without re-zeroing or resource
preflight. Call 6, ELF image quota, user regions/L2 limits and recovery floor
remain unchanged. Before first allocation the core reserves missing framebuffer
bytes/one region/two L2 tables and 3 MiB unused physical pool.

The production dispatcher uses a private copied record, the supervisor owns the
epoch, and uspace passes exclusive canonical pool/table/counter views into the
accepted append-only transaction core. No transaction function services I/O,
retains a user pointer, executes user code, maps an intervening object or calls
an external borrower. It zeroes all new pages before publishing PTEs. After
prepare, queued STOP cancels and rolls back before copy-out. A failed copy-out
validates the complete destination before any store, then rolls back the new
append. With stable mappings the successful span loop cannot newly fail.
After successful copy-out, finish publishes the lease; its bounded descriptor
validation is infallible for the untouched canonical frontier. Corrupt-frontier
behavior is the inherited fail-closed transaction contract, not a recoverable
hardware-partial-store atomicity claim. No concurrent/IRQ ownership is added.
A STOP arriving after commit is handled at the main-loop boundary before user
re-entry, through the accepted Slice B teardown path.

Reset checks all three reserved arena L1 sections against the privileged
mapping. It does not broaden `us_map` into a general heap mapper. Usable mappings
are created only by the checked dedicated transaction. Exit/fault/restart,
failed launch and STOP retain the accepted epoch revoke/quarantine/captured-use
4096-byte scrub-before-reuse lifecycle. Epoch exhaustion remains launch-only
EOVERFLOW75 and never wraps. Numeric pointers are not generation-tagged.

`tests/arena_client.c` is a synthetic NON-AUDIO SDK program for ARM build/loader
qualification, not a boot/default app. Native SDK transport tests link actual
production supervisor, dispatcher, loader and uspace with existing hardware/media
boundaries. HOST_TEST is explicit; no GNU-only dead-strip flag is required.
Linux host execution and ARM cross-compilation do not constitute macOS execution,
physical RAM/MMU/cache/TLB correctness, real guard faults or timing acceptance.
The <=50ms first-acquire service gap and <=2ms cleanup quantum remain explicit
Mac-owned physical gates. Do not install without separate authorization.

Original implementation/qualification: Lewis Moten with Hermes Agent,
OpenAI Codex GPT-6.1 Sol (`openai-codex`). No new third-party algorithm or audio
implementation is adapted; sibling source and NOTICE credits are preserved.
