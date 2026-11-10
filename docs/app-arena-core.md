# App arena slice A — historical unlinked qualification

**Current status:** Slice B binds lifecycle/cleanup to the supervisor and
uspace; Slice C exposes allocation through ABI 1.6 call 21. The current ABI is
1.10. See [app-arena-supervisor.md](app-arena-supervisor.md) and
[app-arena-abi.md](app-arena-abi.md). Hardware timing/MMU acceptance remains
hardware-untested.
The remainder below records the original Slice A boundary, not the current link list.

Original MIT implementation by Lewis Moten with Hermes Agent / OpenAI Codex
GPT-6.1 Sol (`gpt-6.1-sol`, `openai-codex`). No third-party implementation was
adapted. Descriptor encoding follows the existing `core/uspace.c` RW/XN pages.

`core/app_arena_core.c` is deliberately absent from every runtime, selector,
supervisor, syscall, SDK and program link list. There is no live ABI 1.6 or call
21, no startup extension, no allocator/loader/quota change, and no arena user
mapping reachable from the running kernel. Do not install this as a feature.
The borrowed arrays are only host fixtures in this slice. B/C require separate
operator approval after independent review and acceptance of A.

## Ownership contract for a future, separately approved binding

`aa_space` is a *transient borrowed view*, not another allocator or pool. Its
pointers must name the existing full-size pool/L1/L2/region/section arrays and
its counters must be their actual current append frontiers. Every pointer is a
trusted kernel pointer, never a user-provided address. Callers guarantee valid,
nonaliasing storage, a canonical existing mapping prefix, zero unused metadata,
and exclusive serialized ownership. `l2_pa` is the actual physical base of the
contiguous existing L2 array. Counts must be transferred back only by an explicit
binding, which this slice does not supply. Native stand-ins are not board PAs.

No callback, `service_io`, competing mapper, user execution, or MMU table
activation may occur between prepare and finish. The caller must already have
restored the privileged table before quiesce, block user entry throughout
cleanup, and implement the existing ARM maintenance before any future exposure
or revocation. This module does not close handles, alter saved frames, switch
MMU tables, flush caches/TLBs, observe STOP, or implement restart policy.
Host success cannot qualify those absent supervisor/architecture paths.

The bounds checks reject impossible counts/alignment/physical L2 extents; they
are not a validator for arbitrary corrupt kernel pointers or forged mapping
metadata. In particular, `aa_space` and `aa_cleanup` must not be user-writable.

## State and transaction

Zero-initialize the owner once at cold boot. `aa_begin` issues a nonzero 64-bit
app epoch and enters LAUNCHING; active or dirty IDLE owners return -16. Last
UINT64_MAX is terminal -75, never wrap. `aa_run` enters RUNNING; a future launch
binding may call it only after successful launch and a final STOP check.

`aa_prepare` validates running state (-16), exact nonzero epoch (-9), then
minimum request 1..2,195,456 (-22), in that order. An already published lease
returns **1**, without clearing data or checking additional capacity. First
prepare returns 0 and a 16-byte checkpoint, zeroes exactly 536 pool pages,
appends 3 dedicated L2 sections and one RW/XN region, charges the fixed cap
(not the request), and sets PENDING. It does not publish owner PA/bytes yet.
Preflight preserves 3 MiB pool floor and the absent framebuffer's 1,228,800
bytes / one region / two L2 slots. Conflicts or insufficient resources return
-12; invalid borrowed bounds return -22. All dedicated arena sections must be
empty, with conservative rejection of any nonzero preceding guard section.
The latter is stricter than merely requiring its final PTE empty, intentionally
safe for the dedicated address policy, not region/table widening.

The 32-byte owner, 16-byte checkpoint and 64-byte cleanup record match the
approved budgets. The view itself is additional transient call-frame storage:
36 bytes ARM, 56 bytes native x86-64. The module has no permanent allocated
objects, no new tables and no second physical pool. Stack usage is compiler-
specific and must be reported separately from these structure budgets.

`aa_finish(..., true)` checks the exact append frontier, region and every new
L1/L2/PTE, then publishes lease PA/bytes. `false` models a failure **before any
caller output store**: it removes only the dedicated append, clears appended
metadata, scrubs its pages, restores counters/state. Existing live mappings and
bytes are untouched. No actual syscall record/copy-out is implemented here.
A changed frontier or descriptor is -16 and leaves PENDING intact, never
rewinds someone else's mapping. A checkpoint is trusted, single-transaction
storage, not a durable epoch token. It must not outlive or be reused across
transactions; rejected prepares do not initialize it. Idempotent return 1
requires no finish. Persistent corrupt-frontier PENDING is fail-closed and
requires external recovery/disposition; it is not a successful rollback.

## Teardown core, not integrated lifecycle

`aa_quiesce` rejects PENDING; otherwise it revokes active epoch/lease before
capturing the full used extent. It verifies owned L1 descriptors before clearing
those entries. Invalid views stay QUIESCING with sticky error and no active
epoch. A repeated quiesce cancels pending restart disposition/path without
rewinding the cursor. A future caller may set its desired terminal intent after
the initial quiesce; repeated STOP cancels it. Timestamp/exit fields are caller-
owned storage only, not interpreted here.

`aa_clean_step` scrubs at most 4096 bytes, returns 0 for more work, and returns
1 only after the entire captured used extent is zero and user L2/region/section
metadata/counts are reset. The final step also clears the bounded metadata
arrays. It preserves last-issued epoch and unrelated privileged L1 entries.
A changed extent, invalid cursor/view or previous cleanup error fails sticky
(-22), never allowing a new launch. Calls outside QUIESCING return -16.

Tests exercise failed-launch partial extents, empty idle STOP, mapped teardown,
repeated STOP, stale generations and gated launches, not actual OS exit/fault/
I/O callbacks. Those routes, policy decisions and frame/handle revocation belong
to B. First-fill 50 ms / per-quantum 2 ms remain **unmeasured physical targets**.
No ARM execution, emulator retry, timing acceptance or cross-app MMU claim.

## Reproduction and coverage

`make test` discovers `tests/test_app_arena_core.py`, which builds and executes
both native harnesses against the actual production unit. `app_arena_maps.c`
includes unmodified production uspace in a test-only translation unit to access
its private counters, constructs mappings with real `us_map`, and uses
`us_span`/`us_check` to verify guards. It is not a runtime binding. No Makefile
link source changes are needed.

- Native unit: epochs/no-wrap, request/error order, idempotence, full fixed
  zero-fill, all PTEs/guard padding, checkpoint/frontier failures, rollback,
  teardown/quarantine, 128 reuse cycles and full 16 MiB/4096-quantum cleanup.
- Actual-uspace harness: 8 rollback cycles per framebuffer ordering, exact prior
  mapping/byte preservation, all 8 regions/16 tables at maximal mandatory maps,
  missing-framebuffer reserve, legal sparse 4-segment/8 MiB ELF-layout L2
  exhaustion with 64 unchanged failures. This builds legal layout maps, **not
  actual ELF-loader execution**. Separate internal us_map pool/region stress
  exceeds ELF quota/segment constraints and is explicitly not app admission.
- Compile-only `app_arena_layout.c`: native + ARM field offsets and budgets,
  existing FB_SURFACE_MAX equality, target transient-view size.
- Dedicated ASan+UBSan harness builds instrument both new production unit and
  the included real uspace; leak detection enabled. Existing kernel `test-asan`
  is normal tests plus targeted LFN instrumentation, not all-core sanitization.
- Disposable source mutants must fail assertions, not compile. Mutation scripts,
  full raw runs, ARM objects/stack output, unchanged-runtime image and signed
  bundle closure accompany the private task handoff, not this source tree.

No app, SDK or ABI advertisement is added. No audio/VOC, quota, hardware,
installation, publication or acceptance follows from these host tests.
