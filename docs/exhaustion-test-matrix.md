# M1 bounded host resource-exhaustion matrix

Final branch results and shared portability fixes are recorded in
[M1 host acceptance integration](m1-host-acceptance.md). Baseline results below
are retained as evidence of the standalone test contribution.

Scope: M1 acceptance tests 3–5 in `docs/milestones.md`, the slice-2 limits
in `docs/m1.md`, and the original request's handles, user pool and log-flood
requirements. These are host checks of real implementations, **not** physical
ARM, USB, watchdog or preemption qualification. No production code is changed.

Run the nine added tests with:

```sh
python3 -m unittest discover -s tests -p test_exhaustion.py -v
```

`tests/test_exhaustion.py` reuses the existing per-test copied shared library,
FAT fixture and syscall helpers without inheriting their test cases. All normal
core builds still go through `tests/hostlib.py`; it is unchanged. A separate
exhaustion-owned C executable includes the real `drivers/usb.c`, using synthetic
TinyUSB descriptors and CDC credits. It does not exercise descriptor validity,
TinyUSB internals, USB enumeration or the physical controller.

## Requirement → test

Names below refer to methods in `Exhaustion` unless prefixed otherwise.
Every campaign repeats four cycles with fixed counts and no randomness/sleeps.

| Requirement / finite resource | Test | Boundary, failed state and reuse |
| --- | --- | --- |
| M1.4/5: 16 program handles (`SYS_HANDLES`) | `test_program_handles_close_reuse_and_reset` | Allocate all 16, reject the next three opens with `-EMFILE` (-24) and an unchanged zero-output open record. Verify the live pool digest, region/L1/L2 bytes and handle count, and use every existing handle after each refusal. Close a varying slot, reuse it with a new id, reject the stale id with `-EBADF` (-9), then release all handles. Reset also revokes a live id. |
| M1.5/7: eight underlying asset handles (`ASSET_HANDLES`) | `test_asset_subquota_failure_does_not_leak_program_slots` | Open eight files; 24 more opens fail `-EMFILE` with zero output fd and no program-slot leak. Each original file still starts at byte zero. Close/reopen one asset, reject its stale id, fill all eight remaining program slots with log handles, and prove reset releases both handle layers for the next cycle. |
| M1.5: 16 MiB physical user page pool (`U_POOL_BYTES`) | `test_pool_exact_limit_atomic_failure_and_reset_reuse` | Map exactly 4096 pages. Each of three following one-page maps returns `-ENOMEM` (-12), `USER POOL`, preserving accounting, every descriptor/region and a SHA-256 of all live pool bytes. Kernel identity map remains unchanged and kernel addresses remain inaccessible. Reset revokes mappings, resets accounting and allows the whole pool to be reused, with all reused bytes zeroed. |
| Finite mapping metadata: eight regions (`U_REGIONS`) | `test_region_limit_atomic_failure_and_reset_reuse` | Map eight adjacent one-page regions using only one L2 table. Next maps fail `-ENOMEM`, `USER MAP REGIONS`, with identical mappings/accounting/live bytes. Reset permits a mapping at the formerly refused address. |
| Finite mapping metadata: 16 coarse page tables (`U_L2`) | `test_l2_limit_with_pool_space_left_is_atomic` | Use all 16 L2 tables while leaving pool bytes and region slots available. A new section fails `-ENOMEM`, `USER L2`, without state mutation; a new page in an existing section succeeds. Reset allows the full campaign again. |
| M1.4/5: allocation failure through an actual syscall | `test_fb_mmap_exhaustion_preserves_record_and_recovers_on_restart` | Fill the region quota in a running fixture, then repeat framebuffer `mmap`: `-ENOMEM`, zero output address and unchanged entire request record. Restore only the intentionally written request before comparing all live memory/mapping state. Fault cleanup revokes handles; fresh load successfully maps the framebuffer. |
| M1.5: preserve recovery under simultaneous exhaustion | `test_fault_recovery_after_simultaneous_pool_and_handle_exhaustion` | Fill both handle layers and the entire pool, refuse the next open/map, inject a user fault, follow the existing restart/stop policy to the recovery console, and verify handles revoked, kernel map unchanged and kernel HDMI border restored. Reload DIAG, verify the exact original pool usage, reject all stale handles and read an asset again. No stall/input policy is changed. |
| Original request: user log flood | `test_log_syscalls_remain_bounded_and_leave_other_resources_usable` | 512 writes per cycle (2048 total), each with more than 120 bytes. Transfers retain the existing success/count contract; captured lines are capped at 120 payload characters. Pool use and handle count do not drift, and an asset can still be opened/read/closed each cycle. The shared log sink is explicitly not the real USB queue. |
| Original request/M0.3: bounded real log queue and recovery reply reserve | `UsbLogExhaustion.test_real_queue_slow_reader_and_recovery_reply_reserve` / `tests/exhaustion_usb.c` | Fill the actual 4096-byte ring exactly with 32 lines. Next log increments `dropped` without touching queued bytes or cursors; 1024 more rejections remain bounded. With zero TX credits, 32 service calls return without draining. The independent reply queue delivers `OK recovery` while logs remain full and protocol activity suppresses log drains. Disconnected-reader behavior, 4096 one-byte drains, exactly one drop report, empty drain and successful new logging are asserted. |

## Explicit gaps and supported release semantics

- General user allocation/anonymous `mmap` is not implemented in slice 2
  (`core/sys.c` returns `-ENOSYS` for the anonymous sentinel). Full-pool and
  mapping-metadata exhaustion therefore call the real internal `us_map` API;
  they do not claim an ELF program can allocate 16 MiB through the public ABI.
  Public framebuffer allocation failure is tested separately.
- There is no individual page-unmap API. `us_reset` and OS reload are the
  supported page release/reuse paths. Closing a framebuffer handle deliberately
  leaves its surface mapped until the program ends.
- The 8 MiB ELF image quota is a loader rejection requirement, not the physical
  pool size. Existing ELF tests already reject an over-quota image; the ELF
  acceptance lane owns deeper quota/segment/parser boundary coverage.
- Recovery storage is statically separated: the pool has a fixed physical
  range, privileged mappings survive, and USB reply/log arrays are independent.
  The implemented host tests demonstrate preserved state and usable recovery
  paths; there is no runtime general-kernel-allocator reserve counter to assert.
  They do not certify all board memory ownership or a measured recovery budget.
- A physical ARM fault, two-tick preemption, stalled heartbeat, SD/panel crashes,
  OS A/B fallback and real USB progress remain distinct acceptance gates. The
  injected fault here exercises real supervisor cleanup, not hardware exceptions.
- Log overflow intentionally drops lines; it does not return an exhaustion errno
  to `write`. Successful writes still report the requested transfer count.

## Validation and provenance

Baseline: fresh `origin/main` at
`aac205c699c26a98641894dadd4ba97e6964115b`. Runtime: Hermes, GPT-6.1 Sol
(`gpt-6.1-sol`), provider `openai-codex`, verified against the active session's
persisted model and billing-provider fields; no fallback used.

On the Linux validation host the unchanged baseline build is not directly
portable: GCC treats existing compact statements as misleading indentation, and
ELF symbol interposition resolves the kernel's `error()` name to glibc unless
local binding is requested. Validation therefore used an **external, uncommitted**
`cc` wrapper executing `/usr/bin/cc -Wno-misleading-indentation -Wl,-Bsymbolic`
with the original arguments. macOS `fsck_msdos` is absent; an external wrapper
executes `fsck.fat` unchanged and preserves its exit status. No checker error is
suppressed. The USB-only executable additionally requests the indentation flag
itself; the shared host build remains untouched.

With those compatibility commands, all nine added tests pass. The final full
run executes 91 tests in 14.873 s with the two baseline failures and one skip;
the explicitly filtered regression run omitting only those two known failures
executes 89 tests in 13.852 s, passing with the same one skip. The isolated USB
campaign also passes AddressSanitizer and UndefinedBehaviorSanitizer with leak
detection enabled. Five disposable
source mutants are rejected by the corresponding tests: wrong handle errno,
wrong pool errno, region-failure accounting mutation, L2-failure descriptor
mutation and rejected-log byte mutation. Mutants never touch the work checkout.

The 82-test existing baseline independently reproduces two failures in
`Install.test_create_replace_and_checks` and its inherited `Protocol` counterpart:
`fsck.fat` returns 1 because the boot label `SANDBOX M0` has no root-directory
volume-label entry. The ARM compiler-dependent test is skipped because that
compiler is unavailable. Full-suite validation retains these same baseline
failures; it is **not** reported as fully green. No kernel implementation defect
was found by the new exhaustion campaigns. Integration owns shared build/checker
portability, NOTICE attribution and final publication on
`test/m1-host-acceptance`.
