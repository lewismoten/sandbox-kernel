# M0 control-record host properties

## Scope and seams

`test_m0_control_properties.py` adds 17 auto-discovered unittest methods. It uses
`tests/hostlib.py` and the existing fake block-media harness, with no production,
shared harness, registration, or other subsystem changes.

Record-local campaigns call the real `control_healthy()`: the candidate is written
to the selected on-disk copy and mirrored into the existing exported `control`
array, so disk/RAM equality does not mask semantic or checksum failures. This
exercises the private `record_valid()` without exporting it. A separate test
requires exact disk/RAM equality. Each external disk edit closes/reopens the C
stdio stream to avoid buffered stale data.

Ordering, conflict, recovery, sequence exhaustion and durable alternation use
`store_init()` and `control_commit()`. `slot_load()` independently verifies the
meaning of a record's otherwise opaque slot hash. These are the store interfaces
used by `core/selector.c`; the ARM selector entry, MMIO, actual trial execution,
physical SD programming and power-loss durability are **not** executed on the
host. Attempt values 0/1 and pending state are covered without pretending to run
the board selector.

The fixture is independently serialized with literal documented little-endian
fields and offsets, and compared with `scripts/image.py:control_record`. The
oracle uses Python `zlib.crc32`, not the C CRC implementation; both CRCs are also
checked against the ISO-HDLC `123456789` vector (`0xcbf43926`). Random campaigns
compare outcomes with the independently stated wire rules, not a blanket
"mutations must fail" assumption.

## Invariant matrix

| Requirement | Test/witness |
| --- | --- |
| 512 bytes, magic, format, little endian, CRC over bytes 0–507 | Independent fixture, known CRC vector, field cases and every-bit campaigns |
| Every individual bit including all 32 CRC bits | Two accepted fixtures, exactly 4096 flips each, no repaired CRC; 8192 rejections |
| Sequence validity | Zero and maximum sequence rejected with repaired CRC; valid sequences at 1, 2, signed boundary and `0xfffffffe` |
| Confirmed/pending slot and attempts | Invalid values, same-slot pending, absent pending with consumed attempt; both slots and both attempt states accepted when applicable |
| Generations | Confirmed zero rejected; pending zero/equal/lower rejected; unsigned signed-boundary crossing accepted; exhausted-generation wrap rejected by commit |
| No-OS sentinel | Correct sentinel accepted; repaired-CRC alternatives rejected |
| Reserved zeros | Every word 9–15 and 32–126 set independently to 1, high bit, all ones, then resealed |
| Absent pending metadata | Each pending-hash word independently made nonzero, nonzero pending generation and consumed attempt rejected |
| Hash validation separation | Zero/opaque hashes allowed as record content; a checksummed but wrong confirmed hash fails real `slot_load()` |
| Newest valid record wins | All 36 ordered pairs of six boundary sequences, plus 128 seeded pairs with unrelated generations |
| Invalid newest copy fallback | Corrupt newer records in both positions; independently invalid semantic copies in both positions |
| Equal sequence | Identical copies accepted; distinct valid records rejected as `CTRL CONFLICT`, in either position |
| Both invalid | Mount permits metadata-free recovery, clears selected metadata; commit stays locked, no writes |
| Commit alternation | Eight transitions across both initial selected positions; complete new record read from older-copy target and previous record preserved |
| Sequence exhaustion | `0xfffffffd` commits to `0xfffffffe`, next commit rejected without writes or RAM changes; no serial-number wrap ordering |
| Commit semantic rejection/failure | Repaired invalid proposals rejected before writes; injected media-write failure preserves selected record and both copies |
| Replay information | Seed/iteration and complete hex inputs for randomized assertions; bit index and full record for exhaustive assertions |

The 365 repaired-checksum semantic rejections comprise 23 header/field cases,
330 reserved/absent-hash cases, eight lower/equal pending generations, two
same-slot cases and two invalid attempts. The valid boundary matrix contains
160 combinations. CRC failures cannot explain these semantic rejections.

## Documentation gaps and policy boundaries

`docs/m0.md` specifies the layout, initial sequence/generation 1, unsigned
little-endian fields, rejected wrap, reserved zeros, CRC, authoritative newest
sequence, and equal-sequence conflicts. It does **not** fully enumerate all
numeric admissibility rules. The following tests explicitly characterize
`core/store.c` policies rather than silently claiming stronger published rules:

- Sequence 0 and `0xffffffff` are invalid; `0xfffffffe` is readable but terminal
  for `control_commit`. The document does not name these numeric endpoints.
- Confirmed generation must be nonzero. Pending generation must be strictly
  greater than confirmed. With no pending slot, generation/attempt/hash must be
  zero. `NO_SLOT` is represented by `0xffffffff`.
- Generation `0xffffffff` is locally accepted (as confirmed, or as pending above
  `0xfffffffe`), unlike sequence `0xffffffff`. It cannot wrap to a valid pending
  generation. The updater additionally refuses to advance confirmed generation
  `0xfffffffe`; this test component does not redefine that updater policy.
- Confirmed hashes and present pending hashes have no record-local nonzero
  constraint. Slot content verification is separate.
- Selection compares valid sequences as ordinary unsigned integers, not signed
  or modular serial numbers. There is no documented cross-copy generation
  monotonicity rule, so tests do not invent one: sequence remains authoritative
  even if its confirmed generation is lower than another valid copy's.

No production implementation defect was demonstrated in this control-specific
scope. Numeric endpoint/documentation precision remains an integration note,
not a production fix bundled with these tests.

## Reproducible campaigns and actual results

Fixed local `random.Random` instances; no global RNG state or external corpus:

| Campaign | Seed | Cases | Accepted | Rejected |
| --- | --- | ---: | ---: | ---: |
| Valid checksummed combinations | `0x4d304354` | 1024 | 1024 | 0 |
| Raw byte mutations, unchanged CRC | `0x4d304154` | 2048 | 0 | 2048 |
| Byte mutations with repaired CRC | `0x4d304254` | 2048 | 55 | 1993 |
| Store selection pairs | `0x4d304054` | 128 pairs | 128 selected correctly | 0 mismatches |

Every case is replayed by the normal test command; randomized failure messages
include seed and iteration plus the entire candidate (both inputs for selection).
A reversed-sequence-order mutant failed at selection seed `0x4d304054`, iteration
0. Its failure output was checked to contain both complete 512-byte hex records.

Run from repository root:

```sh
python3 -m unittest discover -s tests -p test_m0_control_properties.py -v
PYTHONPATH=tests python3 -c 'import unittest; import test_m0_control_properties as m; names=unittest.defaultTestLoader.getTestCaseNames(m.ControlProperties); r=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(m.ControlProperties(n) for n in reversed(names))); raise SystemExit(not r.wasSuccessful())'
make test
```

Recorded against base `a17854357950eec18f7f25c8b19a778c5a41b1f5`:

- New tests: 17 pass in normal order and 17 pass in reverse order.
- Existing M0 tests: all 14 pass as part of the full suite.
- Pristine base: `make test` runs 149 tests, 148 pass, one existing Arm GNU
  toolchain skip, zero failures/errors.
- With this component: `make test` runs 166 tests, 165 pass, the same toolchain
  skip, zero failures/errors. No PATH/compiler/checker adapters were used.
- Earlier sibling baseline findings about Linux host linking and FAT label checks
  are already fixed in this newer base; they were not fixed by this component.

## Mutation sensitivity

Twelve disposable source-copy mutants were all caught by **assertion failures**,
not compile/import errors. No deliberate fault is committed:

| Mutant | Detecting test |
| --- | --- |
| Omit magic check | Repaired-checksum invalid fields |
| Omit format check | Repaired-checksum invalid fields |
| Allow sequence zero | Repaired-checksum invalid fields |
| Omit attempt upper bound | Repaired-checksum invalid fields |
| Allow equal pending/confirmed generation | Repaired-checksum invalid fields |
| Ignore absent pending hash | Repaired-checksum invalid fields |
| Ignore reserved tail words | Repaired-checksum invalid fields |
| Ignore CRC | Exhaustive single-bit flips |
| Compare generations as signed | Generation boundary commits |
| Reverse sequence ordering | Seeded selection pairs |
| Ignore equal-sequence conflict | Sequence/conflict matrix |
| Ignore RAM/media identity | Healthy exact-identity test |

Mutation substitutions and their actual assertion outputs are retained in the
component's external evidence archive, not in production or shared test files.
Final combined integration, NOTICE attribution, USB framing scope and safe
publication belong to the integration task.
