# Supervisor reclaim — historical Slice B boundary

Current status: Slice C now exposes the app arena through ABI 1.6 call 21 and
a 48-byte startup extension; the current ABI is 1.11. See
[app-arena-abi.md](app-arena-abi.md). The lifecycle below remains in use;
statements about unavailable allocation describe the original B-only slice.
Physical cleanup timing and MMU/cache/TLB behavior remain hardware-untested.

Original MIT work: Lewis Moten with Hermes Agent / OpenAI Codex GPT-6.1 Sol
(`gpt-6.1-sol`, `openai-codex`). No third-party implementation adapted.

## Runtime boundary

The runtime now links only the lifecycle portion of `app_arena_core.c` via
`core/os.c` and the exclusive `core/uspace.c` cleanup binding. Allocation
prepare/finish/preflight remain unreachable and are garbage-collected from the
runtime. No syscall21, ABI1.6, startup extension, SDK change, app arena mapping,
audio path or new allocator is supplied. The old 40-byte ABI1.5 startup, call6,
ELF8MiB quota, 16MiB pool, 8 regions and 16 tables are unchanged. GMOS/JS/DIAG
remain old applications; no app source changes are needed.

The 32-byte owner and 64-byte cleanup record are kernel static storage, not pool
allocations. `aa_space` is a transient canonical view of the actual pool, tables,
section IDs and counters. `us_clean_step` commits its resulting counters back;
no separate pool or permanently cached borrowed view exists.

## State and ordering

- `os_start` rejects RUN unless IDLE and no STOP is pending. LAUNCHING is set
  before opening/reading media, blocking nested launches through SD service.
  Pre-validation rejection releases that gate without consuming an epoch.
  Recovery-protocol RUN checks the same mutation-free `os_can_start` predicate
  before resetting the restart budget or canceling autostart. Busy rejection
  therefore preserves pending RESTART/DIAG intent, including sticky cleanup
  errors. Explicit STOP still cancels that intent; an admitted fresh RUN still
  starts a fresh budget and disables automatic DIAG fallback.
- After valid ELF planning, `aa_begin` issues the next distinct nonzero u64
  epoch. Failed construction consumes it, but never reaches RUNNING. Last-issued
  is never reset by syscall reset, STOP, cleanup, restart or fallback. Exhaustion
  returns -75 without wrapping or rebooting; recovery remains available.
- The ELF/stack/startup maps and startup prefix are constructed with the existing
  functions. STOP is checked after media I/O and after framebuffer mode service,
  immediately before marking RUNNING. No epoch is published in startup in B.
- Every terminal path clears running/saved frame, revokes the epoch and captures
  the complete used extent before reset. The caller is the safe privileged
  main-loop boundary after `user_enter` restored kernel TTBR/SP; a queued STOP
  during a syscall/SD read waits until that call has finished.
- Quiesce verifies and removes owned user L1 entries. Handles close before page
  cleanup. No user entry, fresh launch or pool reuse is allowed during quarantine.
  Each `os_task` pass scrubs at most 4096 payload bytes; the last also clears the
  bounded L2/region/section metadata and then restores the privileged template.
  Main-loop USB/panel/watchdog work may run between passes, without user callbacks.
- Normal exit retains the last public displayed frame; fault and STOP restore
  the existing kernel home screen. A clean-exit message describes termination,
  not completed scrub. RUN can still return EBUSY while cleanup progresses.
- One-restart/60-second policy is decided at the original fault time, with its
  path and intent retained until scrub completes. Failed partial restarts and
  initial autostart construction defer DIAG until their own rollback completes.
  A failed DIAG launch is cleaned but does not retry forever.
- STOP during cleanup cancels every restart/fallback intent without rewinding
  the cursor or changing the initial STOP result. Repeated STOP is idempotent.
  Pending stays true until successful cleanup or a reported cleanup error.
- Descriptor/view/extent errors stay quarantined, with no early reuse. A repaired
  privileged-template conflict can be retried by a later STOP **only when the
  entire captured extent is already scrubbed and the actual used count is zero**.
  This does not release dirty pages after a descriptor/extent failure.

The signed exit status is retained unchanged in the cleanup record. Logging
keeps the existing low32 magnitude format, but computes it unsigned so INT64_MIN
cannot overflow. This is not a new full64 diagnostic formatting API.

## Verification and limitations

`make test` discovers real production supervisor+ELF+uspace tests. Historical
regressions now perform explicit bounded main-loop cleanup passes before
asserting restart/STOP completion, not a replacement supervisor model. New
quarantine tests deliberately inspect the intermediate states before those
passes. Test-only translation units include production source to observe private
epoch/cleanup state and inject failure at actual map-call boundaries. Fake media,
framebuffer and privileged table are external host boundaries; ARM instructions,
MMU effectiveness and real cache/TLB behavior are not executed there.

```
python3 tests/test_supervisor_reclaim.py
python3 tests/test_supervisor_native.py
SUPERVISOR_SANITIZE=1 python3 tests/test_supervisor_native.py
SUPERVISOR_SANITIZE=1 SUPERVISOR_OLD_ELFS=/absolute/old-elfs python3 tests/test_supervisor_native.py
make test
make test-asan
```

The standalone native executable links actual production sources under ASan+
UBSan with leak detection enabled; no instrumented library is loaded into Python.
It covers actual loaded old ELF/startup/recovery paths, partial construction,
all exception/exit/readiness/heartbeat/sleep routes, full-pool quanta and epoch
exhaustion. The optional old ELF directory tests actual binaries through the
production loader but does **not** execute user ARM code. Existing kernel
`test-asan` remains normal tests plus targeted LFN instrumentation; companion
GMOS sanitizer scope remains its existing configuration, not global leak coverage.

ARM compilation, exact native/ARM budgets, stack evidence, actual old-app loader
checks and fixed-ID fresh-build reproducibility are separate recorded gates in
the private signed handoff. None proves physical acceptance. Real RAM/MMU/cache/
TLB, STOP/panel/watchdog responsiveness and <=2ms cleanup quanta remain Mac-owned
physical gates. Slice C now implements first arena acquisition; its <=50ms
physical service-gap target remains unmeasured. No installation or emulator retry
is implied by this historical report.
