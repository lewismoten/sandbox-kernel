# M1 host acceptance integration

Validated on November 7, 2026, based on `origin/main`
`aac205c699c26a98641894dadd4ba97e6964115b`. Host-only; no board qualification
is implied. The stall rule, input implementation and `core/os.c` were unchanged
by that integration. Current discovery (2026-11-10) finds 440 test cases; the
119-test result below is historical, and optional-tool skips depend on the host.

## Added coverage

| Area | New test methods | Evidence |
| --- | ---: | --- |
| ELF validator/loader | 9 | [Rejection inventory and boundaries](elf-host-coverage.md): 71 deterministic witnesses for all 38 validator reasons; 60,000 mutations plus 40,000 program tables, in addition to the original 10,000 cases |
| System calls | 17 | [Requirement-linked matrix](syscall-acceptance.md): all implemented call envelopes, pointers/counts, output buffers, reserved fields, unknown calls/ioctls, handles, query sizes and major mismatch |
| Exhaustion | 9 | [Quota and recovery matrix](exhaustion-test-matrix.md): both handle layers, 16 MiB pool, region/L2 metadata, syscall allocation failure, fault cleanup, log flood and real USB queue/reply reserve |
| Integration regressions | 2 | Shared-library symbol binding in an isolated child process; matching FAT primary/backup/root labels and file layout |

Total: 37 additional test methods over the 82-test baseline. `make test` runs
119 tests successfully at that integration baseline (not the current suite total). The full suite also passes reverse and seed-1107 shuffled
order. Each run skips the existing ARM layout/svc test because the Arm GNU
toolchain is not installed. Normal validation uses the system
compiler and a real `fsck.fat -n`, with no compiler/checker wrappers, suppressed
checker status, or omitted failing tests. The baseline portability findings in
the individual reports describe their standalone runs, not the final branch.

## Integration defects and fixes

Each defect fix has its regression in the same signed commit:

- On Linux, calls inside the host shared object could resolve kernel `error()`
  to glibc's incompatible variadic function and crash. A bounded subprocess
  regression reproduces SIGSEGV before the fix. The Linux-only host link now
  uses `-Wl,-Bsymbolic`; the board build is unaffected.
- The image builder wrote a `SANDBOX M0` BPB label with no corresponding root
  volume entry. The metadata regression and real Linux FAT checker fail before
  the fix. A root volume-label entry now matches both boot sectors, without
  moving data clusters. Tests select an available macOS or Linux FAT checker,
  preserve its status/output, and fail when neither is installed.
- Four existing adjacent statements triggered GCC's strict misleading-indentation
  diagnostic. Statement separation preserves control flow and keeps `-Werror`
  enabled; no warning suppression was added to the shared build.

No new ELF/syscall/quota implementation defect was found by the acceptance
campaigns. The two fixes above concern the host harness and generated images.

## Independent validation and exact gaps

`python3 tests/elf_coverage.py --deterministic` and
`python3 tests/elf_coverage.py --timeout 120` independently reach 71/71 ELF
executable lines and 149/150 branch outcomes using fresh GCC/gcov counters.
All validator rejection returns are reached deterministically. The only missing
outcome is `core/elf.c:91`, the defensive failed-`us_span` return in `elf_load`:
a validator-produced plan plus successful real `us_map` guarantees the requested
span. This remains explicitly uncovered, not replaced with simulated coverage.

The real USB queue executable also passes AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection enabled. These checks exercise
synthetic transport credits, not USB hardware or TinyUSB internals.

Outstanding milestone gates are not hidden:

- At the ABI 1.0 baseline, no older minor existed. The current kernel is 1.11;
  `test_supervisor_native.py` can load supplied old ELFs, and
  `test_app_arena_native.py` exercises legacy startup/SDK gating on the host.
  Actual older ARM application execution
  on a current kernel remains a separate hardware-untested compatibility gate.
- ARM attributes are not parsed by the current validator. All existing rejection
  paths are tested; that does not implement missing CPU/VFP checks.
- Full-pool exhaustion uses internal `us_map` because anonymous allocation is
  not public. Recovery demonstrates static separation and usable reset/reload,
  not a measured general-kernel-allocator reserve counter.
- ARM User-mode execution, two-tick preemption, faults during physical SD/panel
  work, health/input behavior and OS A/B fallback remain separate board/feature
  gates. This branch does not claim M1 acceptance tests 3–5 complete in full.

No GitHub workflows are added. Publication checks inspect the current default
branch and candidate tree, repository workflow inventory and existing head PRs;
only `test/m1-host-acceptance` is published, never main/master or tags.
