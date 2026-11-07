# ELF host acceptance coverage

## Scope and provenance

- Baseline: fresh `origin/main` at `aac205c699c26a98641894dadd4ba97e6964115b`.
  The repository-specific `main` instruction overrides the generic `develop`
  instruction. No production source or existing/shared test file is changed.
- Runtime: Hermes, GPT-6.1 Sol (`gpt-6.1-sol`) through `openai-codex`.
  Verified against the executing session's model/provider usage record; no fallback.
- Host: GCC/gcov 15.2.0, Python 3.14.9; measured November 7, 2026.
- Added: `tests/test_elf_acceptance.py` (9 test methods) and the optional
  `tests/elf_coverage.py` instrumented runner. Normal discovery uses
  `tests/hostlib.py`; coverage builds the same unmodified ELF/user-space sources
  with `-O0 -g --coverage -fPIC -fno-builtin -DHOST_TEST` in a fresh directory.
- Local implementation branch: `feature/elf-host-acceptance-t_ff741973`.
  The integration worker owns publication of `test/m1-host-acceptance`; this
  worker does not push, merge, change NOTICE.md, or edit shared host infrastructure.

This is host validation, not ARM execution or physical recovery-console
qualification. Expected behavior comes from `core/elf.c`, `include/elf.h`,
`include/uspace.h`, `docs/m1.md` (ELF checks), and M1 acceptance tests 3–5 in
`docs/milestones.md`.

## Reproduce and replay

```sh
# Ordinary host build, including the 100,000-case extension:
python3 -m unittest discover -s tests -p test_elf_acceptance.py -v
# Existing ELF suite (also retains its original 10,000 cases):
python3 -m unittest discover -s tests -p test_m1_user.py -k Elf -v
# Independent deterministic coverage, then deterministic + fuzz coverage:
python3 tests/elf_coverage.py --deterministic
python3 tests/elf_coverage.py --timeout 120
# Example direct replay, proven to execute exactly one input per campaign:
ELF_FUZZ_SEED=1107 ELF_FUZZ_INDEX=12345 \
  python3 -m unittest discover -s tests -p test_elf_acceptance.py -k fuzz -v
# Select only one campaign by using -k mutations or -k tables instead.
# Optional longer budget (per campaign, capped at 1,000,000):
ELF_FUZZ_SEED=1107 ELF_FUZZ_CASES=100000 python3 tests/elf_coverage.py
```

Defaults are 60,000 header mutations and 40,000 generated program tables, seed
1107: ten times the pre-existing total campaign budget, **in addition to** the
original tests. Each input uses its own `random.Random((seed << 32) + index)`;
replay does not require generating earlier inputs. The table campaign intentionally
mixes valid/near-valid layouts with unconstrained fields and uses all 1–16 PHDR
counts; accepted plans must satisfy independent range, permission, overlap,
quota and file-backed entry invariants. The two campaigns are not independent
proofs of rejection-path coverage; named deterministic witnesses provide that.

The coverage runner places its log, raw `.gcno`/`.gcda`, annotated gcov, gcov JSON,
`summary.json`, and a flushed pre-call `fuzz-trace.jsonl` in a fresh
`build/elf-coverage/run-*` directory. The last trace record identifies even a
crash/timeout input. An assertion failure saves its exact bytes under
`build/elf-failures` in normal runs, or the coverage run's `failures` directory;
`ELF_FAILURE_DIR` overrides this. `ELF_FUZZ_TRACE` enables the same checkpoint log
for normal runs. Seed, campaign and index are sufficient to regenerate an input
with this generator revision and Python runtime. No generated files are committed.
The default coverage subprocess has a hard 120-second timeout; counts are bounded
and individual generated inputs are small.

## Measured evidence

| Run | Result | Measured test subprocess time | ELF source coverage |
| --- | --- | --- | --- |
| Deterministic only | 7 tests pass | 0.064 s | 71/71 executable lines; 149/150 branch outcomes |
| Deterministic + seed 1107 | 9 tests pass; 100,000 fuzz cases | 3.020 s | 71/71 executable lines; 149/150 branch outcomes |
| Existing ELF tests | 4 tests pass; original 10,000 cases | 0.756 s unittest time | Not used to inflate the new coverage measurement |
| Direct replay index 12345 | 2 tests pass; exactly 1 input per campaign | 0.701 s unittest time | Mutation rejects `ELF MAGIC`; table accepts |

The campaign accepts 3,207 mutation inputs and 13,132 table inputs; all accepted
plans pass the invariants. The deterministic-only result is already 149/150
(99.33%) GCC branch outcomes, so **no covered rejection depends on random luck**.
Line coverage alone would incorrectly conceal the untaken return on line 91.
The gcov JSON branch index for that missing outcome is 2; annotated textual gcov
numbers it branch 3 because it counts the preceding call as well.

A separate temporary-source sensitivity probe confirmed the deterministic suite
fails against eight deliberately incorrect variants: class reason, disabled W+X,
disabled overlap, disabled quota, incorrect segment-count guard, disabled
relocation rejection, permitted seventeenth PHDR, and zeroed rather than copied
payload. All eight mutant runs exited 1 with assertion failures, not crashes.
These probes do not alter tracked production files or count as additional
production-path coverage.

### Shared host-build caveats

Unmodified `hostlib.py` on this GCC host fails compilation because
`-Werror=misleading-indentation` rejects existing compact statements. A private
compiler wrapper adding `-Wno-misleading-indentation` allowed ELF tests to run.
That flag already appears in the production Makefile's CFLAGS. A full-suite run
then exposed the pre-existing Linux shared-library `error()` symbol interposition:
an isolated M0 test also crashes without importing the new ELF tests. Adding
`-Wl,-Bsymbolic` to the private wrapper resolves that crash.

With those two wrapper flags, `make test` completes 91 tests in 15.326 s but has
two installer errors because `fsck_msdos` is absent; one ARM-toolchain test skips.
Using an honest `fsck_msdos` wrapper executing installed `fsck.fat -n` instead
completes 91 tests in 15.303 s with two installer failures: the existing fixture's
boot label `SANDBOX M0` has no root volume-label entry, so fsck.fat returns 1.
No checker output/exit status is suppressed. These shared portability findings
were handed to the syscall/integration workers; this ELF change does **not** claim
an unqualified green full `make test`. The ELF-only instrumented runner does not
need those wrappers and passes with the system compiler.

## Exhaustive validator rejection inventory

Line references below are to the baseline `core/elf.c`. Every named witness is a
subtest of `ElfAcceptance.test_every_validator_rejection` in
`tests/test_elf_acceptance.py`, unless another method is named. Its returned reason
is asserted exactly. There are **71 named byte-input witnesses covering all 38
validator reason strings**, plus explicit NULL tests. The source-string inventory
assertion detects newly introduced reason strings, but is not itself execution
coverage. Same-string duplicate guards are separately witnessed below.

| Line | Rejection | Deterministic witness(es) |
| --- | --- | --- |
| 21 | `ELF SHORT` | `short-0`, `short-1`, `short-51`; `test_null_and_short_reset_plan` additionally passes NULL with n=52 and verifies the cleared plan |
| 22 | `ELF TOO BIG` | `too-big` supplies a real 1 MiB + 1 byte allocation; exact 1 MiB also accepts |
| 23 | `ELF MAGIC` | `magic-0` through `magic-3`, each magic byte separately |
| 24 | `ELF64`, `ELF CLASS` | `class-2`, `class-0`, `class-3`, covering both ternary return values |
| 25 | `ELF ENDIAN` | `endian` |
| 26 | `ELF VERSION` | `ident-version`, `header-version`, covering each OR operand |
| 27 | `ELF OSABI` | `osabi`; OSABI 0 and historical ARM 97 accept |
| 29 | `ELF PIE`, `ELF TYPE` | `pie`, `type`, covering both ternary return values |
| 30 | `ELF MACHINE` | `machine` |
| 32 | `ELF EABI` | `eabi` |
| 33 | `ELF HARD FLOAT` | `hard-float` |
| 34 | `ELF FLAGS` | `flags`; soft-float flag present/absent accepts |
| 35 | `ELF HEADER SIZE` | `ehsize`, `phentsize`, each operand separately |
| 37 | `ELF PHNUM` | `phnum-zero`, `phnum-17`, `phnum-65535` |
| 38 | `ELF PHDR RANGE` | `phoff-low`, `phoff-truncated`, `phoff-unaligned`, `phoff-overflow` |
| 39 | `ELF THUMB` | `thumb` |
| 40 | `ELF ENTRY ALIGN` | `entry-align` (bit 1, not the preceding Thumb rejection) |
| 47 | `ELF EXEC STACK` | `exec-stack`; non-executable GNU stack also accepts |
| 48 | `ELF INTERP` | `type-3` |
| 49 | `ELF DYNAMIC` | `type-2` |
| 50 | `ELF SHLIB` | `type-5` |
| 51 | `ELF TLS` | `type-7`; also final sixteenth header in `test_maximal_program_headers` |
| 53 | `ELF PHDR TYPE` | `type-4294967295` |
| 56 | `ELF SEG FLAGS` (unknown bits) | `seg-flags-high` |
| 57 | `ELF WX` | `wx` |
| 58 | `ELF SEG FLAGS` (no R/W/X) | `seg-flags-empty`, distinct from line 56 |
| 59 | `ELF FILESZ` | `filesz` |
| 60 | `ELF SEG FILE` | `seg-file`, `seg-file-overflow`; arithmetic computed beyond UINT32_MAX |
| 61 | `ELF ALIGN` | `align-power` (non-power-of-two), `align-congruence` (offset mismatch); alignment 0/1/powers of two also accept |
| 63 | `ELF SEG RANGE` | `range-low`, `range-high`, `range-overflow`; exact window end accepts and one-byte excess rejects |
| 64 | `ELF SEGMENTS` | `segments-five` with valid disjoint pages; four loads plus twelve ignored headers accepts |
| 67 | `ELF OVERLAP` | `overlap-0`, `overlap-16`, `overlap-4095`; reversed disjoint VA order also covers the second comparison's false outcome |
| 70 | `ELF QUOTA` | `quota-rounded`; quota−1 and quota bytes accept, quota+1 rejects after page rounding |
| 73 | `ELF NO LOAD` | `no-load-ignored`, `no-load-empty` |
| 76 | `ELF ENTRY` | `entry-after-file`, `entry-before-text`, `entry-crosses-file`, `entry-not-executable`; pure BSS text also rejects |
| 78 | `ELF SHDR RANGE` | `shentsize`, `shnum`, `shoff-align`, `shoff-eof`, `shoff-overflow`; also exact section-table EOF versus one-byte truncation |
| 81 | `ELF RELOC` | `reloc-4`, `reloc-9`, `reloc-6`, `reloc-11`, `reloc-5`, `reloc-1879048182`; each forbidden type separately, including final 256th section; corresponding non-allocated sections accept |

## Loader rejection inventory and explicit gap

| Line | Rejection | Evidence / reason unreached |
| --- | --- | --- |
| 90 | propagate nonzero `us_map` result | `test_load_copy_bss_padding_and_map_failure` loads a valid plan successfully, then loads it again without reset; real overlap refusal propagates `-22` |
| 91 | return `-14` if `us_span` fails | **Unreached.** For a validator-produced plan, memsz is nonzero, filesz ≤ memsz, the image range is bounded and permissions are nonempty R/W/X. Successful real `us_map` creates the entire rounded region containing va. `us_span(..., U_R, ...)` accepts any of R/W/X in this implementation. It therefore finds the just-created region and cannot return negative without injected failure, state corruption, concurrency outside this single-threaded loader, or violation of its caller contract. We do not fabricate reachability by replacing real user-space functions or supplying unsafe forged plans. |

There are no unreached `elf_check` rejection returns. The loader's line-91
defensive failure remains the sole uncovered branch outcome and is reported,
not counted as tested. Allocation-failure errno varieties inside `us_map` belong
to user-space/exhaustion coverage; ELF verifies the propagation branch itself.

## Boundary semantics and limitations

- Segments: page−1/page/page+1 lengths, starts at page+4 and page−4, exact
  last image byte, adjacent versus shared page, reverse segment order, entry's
  full four-byte instruction in file data (not BSS), quota rounding, copied
  payload across a page, zeroed BSS and both page-padding ends.
- Program tables: 1/15/16 entries, valid four-load maximum, ignored entries
  before/after text, forbidden final sixteenth entry, starts at page−32,
  page−4/page/page+4, and invalid 0/17/65535 counts. The headers may cross a page;
  only alignment and file bounds govern their storage.
- Section tables: starts at page−40/page−4/page/page+4, 1 and maximum 256
  entries, exact EOF and one-byte-short files, every allocated forbidden type
  in the final slot, plus a zero-entry table at EOF. Section table placement
  does not itself allocate user pages. The checker does not validate arbitrary
  section payload bounds, section names, shstrndx or contents; these tests do
  not imply that it does.
- Known documented gap, not a newly discovered defect: `.ARM.attributes`
  contents (CPU/VFP compatibility) are not parsed by this slice. Their segment
  type is ignored. Acceptance test 3's ARM-attribute requirement therefore
  remains incomplete at the production-feature level. Host coverage cannot
  establish ARM instruction legality, board preemption timing or preservation
  of a physical recovery console. No new ELF production defect was found.
