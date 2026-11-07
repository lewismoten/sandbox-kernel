# Host syscall acceptance matrix

Scope: M1 slice 2, baseline `aac205c699c26a98641894dadd4ba97e6964115b`.
The implemented calls are open (1), read (2), write (3), close (4), ioctl (5),
mmap (6), and exit (16). These tests exercise the actual dispatcher, ABI
checker, user-range translator, asset reader and framebuffer code through the
existing host build. They do not execute ARM instructions or establish board
qualification.

## Sources and classification

- **D — documented requirement:** [M1 acceptance test 4](milestones.md#m1-framebuffer-sdfat-protected-os-loading-and-input),
  the [slice-2 syscall contract](m1.md#system-calls), and the published
  [kernel](../include/abi.h) / [SDK](../sdk/include/sandbox/abi.h) layouts.
- **I — implementation-derived robustness check:** a concrete boundary,
  errno or state assertion inferred from [sys.c](../core/sys.c),
  [abi.c](../core/abi.c) or [uspace.c](../core/uspace.c). These are not new
  syscall semantics. In particular, tests do not impose unspecified error
  precedence when several inputs are invalid.

New cases live in [test_syscall_edges.py](../tests/test_syscall_edges.py), class
`SyscallEdges`; test names below are exact method names. Existing cases live in
[test_m1_user.py](../tests/test_m1_user.py), classes `Calls` and `Supervisor`,
and [test_abi.py](../tests/test_abi.py).

## Case matrix

| Applies to | Requirement / boundary | Class | New test method(s) | Existing evidence |
| --- | --- | --- | --- | --- |
| All seven calls | ABI major mismatch, individually nonzero r3–r6, record size limits/multiple of eight, aligned/readable record; exact negative errno | D; I for each selected boundary | `test_dispatch_envelope_for_every_implemented_call` | `Calls.test_record_rules` |
| All seven calls | Header size, flags, both reserved words; each unknown tail byte nonzero rejected | D | `test_header_reserved_and_trailing_bytes_for_every_call` | `Calls.test_record_rules`; `Check.test_rules` |
| All seven calls | Zero eight-byte extension accepted without modifying its tail | D | `test_zero_extensions_are_accepted_for_every_implemented_call` | Existing zero-tail read case |
| Open, read, write, mmap | fd/transferred/address output zero on entry; a readable but read-only record is insufficient for outputs | D | `test_output_fields_must_be_zero_and_records_writable` | `Calls.test_record_rules` |
| Dispatch holes/out-of-range calls | ENOSYS, no record/handle mutation | D; I for all current holes | `test_unknown_dispatch_does_not_consume_handles_or_records` | `Calls.test_record_rules`; `Supervisor.test_svc_paths` covers nonzero svc immediate |
| Open | 64-bit upper address bits, near-UINT64_MAX pointer, 32-bit wraparound, readable prefix followed by hole; zero/256/UINT64_MAX path length | D; I for exact limits | `test_open_pointer_length_and_path_boundary_preserve_state` | `Calls.test_open_rules` covers flags/mode, missing devices/files, embedded NUL, permissions |
| Open | Path ends at last mapped byte; path straddles adjacent readable regions, including read-only suffix | I | `test_open_pointer_length_and_path_boundary_preserve_state` | — |
| Open/ABI | 4096-byte zero-extended record accepted, nonzero last byte rejected; only fd output changes | D; I for maximum size | `test_maximum_zero_extended_record_and_nonzero_last_byte` | ABI header rules |
| Read | Read-only output, writable prefix followed by read-only suffix, read-only prefix followed by hole, upper bits/wraparound/kernel-half pointers; >1 MiB and UINT64_MAX counts | D; I for split-region cases | `test_read_range_failures_do_not_advance_file_or_touch_prefix` | `Calls.test_buffers_and_counts` |
| Read | Failed requests do not advance file position; subsequent full content/EOF correct | I, preserving documented checked-buffer behavior | `test_read_range_failures_do_not_advance_file_or_touch_prefix` | `Calls.test_log_and_sd` covers short read/EOF |
| Read/write record | Buffers and records cross adjacent writable regions; failure on incomplete or read-only record suffix is atomic | D; I for exact region boundary | `test_read_and_record_can_span_adjacent_writable_regions`; `test_record_crossing_readonly_or_unmapped_region_is_atomic` | `Space.test_checks` tests range translator separately |
| Write | Invalid source ranges and excessive counts; read-only source accepted; exact 1 MiB count succeeds and returns full count while logging only 120 characters | D; I for exact maximum | `test_write_boundary_counts_readonly_source_and_log_return` | `Calls.test_log_and_sd` covers sanitization and device direction |
| Read, write, close, ioctl, mmap | Invalid/stale handles rejected; replacement handle remains usable; anonymous mmap sentinel explicitly ENOSYS | D; I for handle encodings | `test_invalid_and_stale_handles_across_handle_calls` | `Calls.test_log_and_sd`; quota campaigns belong to exhaustion tests |
| Framebuffer query ioctl | Small/oversize class buffers, unaligned/high/wraparound/unmapped/read-only pointers, every reserved/output field nonzero; successful output exactly matches mode | D; I for every selected class field/boundary | `test_ioctl_query_envelope_and_outputs_are_atomic` | `Calls.test_framebuffer` |
| Ioctl | Unknown request IDs and wrong handle kinds return ENOTTY without touching class buffers | D; I for wide unknown ID | `test_unknown_ioctl_and_wrong_device_preserve_state` | `Calls.test_framebuffer` |
| Mmap | Zero/oversize/UINT64_MAX length, nonzero offset, wrong protection/flags, executable refusal; one-byte mapping and repeat mapping at same address do not allocate again | D for framebuffer-only RW/shared; I for lengths/reuse | `test_mmap_validation_and_repeated_mapping_do_not_allocate` | `Calls.test_framebuffer` covers other devices and full surface |
| Present ioctl | Not mapped, rectangle outside width/height, huge coordinates/extent all rejected without screen/counter mutation; bottom-right pixel updates exactly once, preserving hardware pitch padding | D; I for endpoint/counter checks | `test_present_rejections_preserve_pixels_and_counter` | `Calls.test_framebuffer`; `Calls.test_bgr_and_no_display` |
| Close, ioctl outer record, exit | Records with no outer outputs may be read-only; query's separate output must still be writable; exit retains signed 64-bit status extremes and returns SYS_EXITED | D; I for signed extremes | `test_input_only_records_may_be_readonly_and_exit_is_signed` | `Calls.test_exit`; `Supervisor.test_exits` covers supervision separately |
| ABI layouts | SDK/kernel sizes/offsets, standalone SDK, emitted ARM svc | D | No duplicate layout test | `Layout.test_host_layout`, `Layout.test_sdk_header_alone`, `Layout.test_arm32_layout_and_svc` |

## Failure preservation and safety

The `reject` helper installs the input before taking a snapshot. Every expected
failure it checks must leave the whole allocated user pool (including records,
output buffers and sentinels), user page tables, handle count, pool usage,
framebuffer pixels and padding, present count, exit status, running status and
media-write count unchanged. A failure must not emit a log. `sys_calls` is
intentionally excluded: dispatch increments it even on rejection. Read-position
preservation is additionally checked by a successful read of known file content;
a handle-count comparison alone would not prove it.

User addresses are integers passed only to the real checked dispatcher. Fixture
writes/reads use `us_span` and only then access its translated host pointer. Tests
split fixture writes at region boundaries rather than accessing an invalid host
address. Large lengths are metadata, not giant host buffers. The exact 1 MiB log
boundary uses a single mapped region and one call, not an exhaustion campaign.
No shared harness file or production code is changed by this test module.

## Reproduction and limits

Targeted host command:

```sh
PYTHONPATH=tests python3 -m unittest test_syscall_edges test_m1_user.Calls test_m1_user.Supervisor test_abi -v
```

The new module has 17 test methods, with table-driven subcases. The targeted set
has 45 tests because `Supervisor` already inherits the original `Calls` tests;
this new class deliberately does not inherit them. On a host without an Arm GNU
toolchain, the existing ARM layout/svc test is skipped, not passed.

Three isolated sensitivity checks remove respectively the ABI-major check, the
transfer-count ceiling, and the reserved-header check. The corresponding new
tests must fail by assertions, not compilation/import errors. These temporary
mutants are not production changes or part of the branch.

Explicit gaps:

- **Older-minor executable compatibility is not established.** This baseline is
  ABI 1.0 (`K_ABI_MINOR == 0`), with no older minor. Zero-extended records exercise
  the documented forward-extension rule, not an older-minor application running
  under 1.1. A real versioned test app must wait for an actual newer ABI; no
  fabricated minor or skip is presented as acceptance.
- No ARM User-mode execution, timer/preemption guarantee, board protection,
  crash-during-I/O, panel refresh, readiness/health/input, or OS A/B fallback is
  established by these direct host calls. Acceptance tests 3 and 5 and exhaustion
  campaigns remain separate workstreams.
- No blanket claim that every internal syscall branch is covered: asset I/O
  corruption, allocation exhaustion and absent-display paths retain their own
  existing/parallel coverage. This matrix is requirement and boundary coverage,
  not a measured branch-coverage report.
- Host portability must be accounted for separately: at this baseline GCC can
  reject existing compressed C formatting under `-Werror=misleading-indentation`;
  the targeted checks were exercised with only that warning disabled via a local
  compiler wrapper, leaving `tests/hostlib.py` unchanged. A full `make test` on
  that Linux host also crashed in the pre-existing M0 bad-manifest test before
  reaching this module. Targeted syscall success is not full-suite success.
