# M0 FAT32 and image property coverage

Scope: FAT32 and image/manifest portion of the host-fuzz request. Companion
control-record and USB framing coverage is summarized in [M0_HOST_FUZZ.md](M0_HOST_FUZZ.md).
No production behavior is changed by these tests.

Original component baseline: `c12402e32eca82dcd284772ab04f0440ff54b1d8`.
Combined integration baseline: `a17854357950eec18f7f25c8b19a778c5a41b1f5`.
The topic branch is `test/m0-host-fuzz`.

## Producer, consumers and oracle

The binary runtime manifest is **not** the JSON sector inventory:

* `scripts/image.py:10-21` produces/validates a 256-byte little-endian SKI1 header
  plus payload. `core/util.c:41-49` is the actual C validator, used by
  `core/store.c:110-121` when loading a slot. `core/selector.c:11-26` selects a
  verified trial/confirmed/recovery slot; selector hardware entry is not run.
* `scripts/assemble_boot.py:15-18` validates the runtime before preallocation;
  `scripts/usb_kernel.py:65-69` validates it before the USB handshake. These are
  callers of the Python validator, not additional manifest schemas.
* `scripts/image.py:33-78` produces the MBR/FAT32 image and `.layout.json`:
  `format`, `sector_bytes`, `partition_start`, `partition_sectors`, and `files`.
  Each file has `first_lba`, `sectors`, `bytes`, `sha256`. No independent JSON
  validator/reader contract exists in production. Tests verify inventory against
  actual bytes and FAT chains rather than inventing JSON rejection behavior.
* `docs/m0.md:68-104` defines header offsets, limits, four mandatory file sizes,
  dual FAT checks, bounded chains, disjoint extents and eight-sector head spacing.
  `docs/milestones.md:61-65,119-121` requires malformed/oversize/hash rejection
  and bounded handling of corrupt filesystem metadata.

A literal 260-byte wire fixture (including a fixed SHA-256 of `abcd`) establishes
header layout without calling the packer. Both validators must independently
accept it. Random producer output is checked against documented byte offsets,
stdlib SHA-256 and exact payload preservation. This is not merely a pack/unpack
round trip or Python-vs-C agreement oracle.

## FAT32 invariant/coverage matrix

Test names below are methods in `test_m0_fat_properties.FatProperties`.
All mounts invoke exported **store_init**, not `initialize`: lost control metadata
can validly leave storage mountable while runtime updates remain locked.
Every probe requires zero media writes and a bounded increase in SD read count.
The fixed fake card has 264192 sectors (`tests/harness.c:20`); the fixture is
created by the real `image.fat_image`, sparse, with no firmware downloads.

| Invariant / implementation | Witnesses | Test method suffix |
| --- | --- | --- |
| Complete readable sector data (`store.c:44,49,60`) | Empty/511-byte MBR, BPB absent/short, directory sector short; reject with exact read diagnostic | `truncated_media_fails_closed` |
| MBR signature and type (`45-46`) | Broken signature/type; both 0x0b and 0x0c accepted | `mbr_and_bpb_boundary_mutations`, `supported_type_fat_tags_and_two_sector_clusters` |
| Partition bounds, subtraction before addition (`47-48`) | Base 0,2047,2049,card end,UINT32_MAX; count 0,min-1,max+1,UINT32_MAX; exact generated bounds mount | `mbr_and_bpb_boundary_mutations`, `generated_layout_inventory_and_read_only_mount` |
| BPB signature, 512 bytes/sector, two FATs, zero FAT16/root/version fields (`50`) | Broken signature; 511/1024 bytes; FAT counts 1/3; nonzero offsets 17,22,42 | `mbr_and_bpb_boundary_mutations` |
| Cluster geometry (`51-52`) | spc 0,3,127,129,255; 128 seeded invalid spc values; reserved=0; FAT 0,count/4+1,UINT32_MAX; flags nonzero; total count +/-1 | `mbr_and_bpb_boundary_mutations`, `seeded_malformed_geometry` |
| FAT32 cluster minimum / sufficient FAT entries (`54`) | Power-of-two spc 4..128 drops below minimum; FAT size 2016 too short; spc 1 and 2 mount with rebuilt cluster mapping; independently relocated sparse FAT media mounts at exactly 65525 clusters and rejects 65524 | `mbr_and_bpb_boundary_mutations`, `supported_type_fat_tags_and_two_sector_clusters`, `exact_fat32_cluster_minimum` |
| Cluster index bounds (`13-18,58,84`) | Roots 0,1,first excluded,UINT32_MAX reject; last usable data cluster maps exactly to final media LBA | `root_chain_termination_and_overlap`, `seeded_valid_fragmented_controls_and_last_cluster` |
| Both FAT copies agree after masking high nibble (`14-19`) | One-copy root/file disagreement rejects; all eight EOC tags and high-nibble tags accepted | `root_chain_termination_and_overlap`, `file_chains_lengths_duplicates_and_overlap`, `supported_type_fat_tags_and_two_sector_clusters` |
| Finite complete root chain, including after first unused entry (`57-71,80-91`) | Deleted-entry-only loop reaches first hop cap; loop hidden after unused entry reaches second cap; repeated named entries reject early | `root_chain_termination_and_overlap` |
| Named entries unique and not directory/volume-label (`62-66`) | Duplicate KERNA, directory/volume attributes, missing KERNB | `file_chains_lengths_duplicates_and_overlap`, `exact_file_size_after_finite_mapping` |
| Finite file chains, valid byte size, exact terminal EOC (`21-30,72`) | Zero, unaligned, oversize, truncated chain, free/reserved/bad cluster, self-loop, one-copy disagreement; independently terminated short slot/control then fail exact size check | `file_chains_lengths_duplicates_and_overlap`, `exact_file_size_after_finite_mapping` |
| Disjoint writable extents (`73-79`) | KERNB shares complete KERNA chain, reject FILE OVERLAP | `file_chains_lengths_duplicates_and_overlap` |
| Root sectors never writable (`80-91`) | Root chain hidden tail enters slot; reject DIR OVERLAP | `root_chain_termination_and_overlap` |
| Control heads at least eight sectors apart (`92`) | Relocate both finite disjoint chains: distances 1..7 rejected; 8 and 9 accepted (not confounded by overlap) | `control_head_spacing_without_extent_overlap` |
| Supported non-contiguous files | 32 seeded fragmented control chains, last cluster included; exact exported sector-map assertion | `seeded_valid_fragmented_controls_and_last_cluster` |
| Inventory describes real media (`image.py:33-78`) | Logical size, partition bounds, signatures, every fixture byte/hash/sector count, both actual FAT chains; second generation byte-identical including sparse zero regions | `generated_layout_inventory_and_read_only_mount`, `generated_image_is_deterministic` |

The valid spc=2 variant tests the restricted mount contract, not a complete fsck
qualification of every BPB advisory field. Reallocated control chains free their
old FAT entries. Tests never treat a rejection as evidence that a valid variant
is supported.

## Manifest invariant/coverage matrix

Methods in `test_m0_image_properties.ImageProperties`:

| Invariant (`docs/m0.md:70-95`; `image.py:10-21`; `util.c:41-49`) | Witnesses | Test method suffix |
| --- | --- | --- |
| Exact v1 profile/ABI/load/entry/size/reserved word and SHA | Literal fixture equals producer output; both consumers accept | `literal_wire_and_producer` |
| Exact producer fields and data, payload 4..MAX_IMAGE-256, printable ID 0..31 | 10 fixed lengths including minimum, odd lengths, sector/ACK boundaries and maximum, plus 128 seeded random lengths | `seeded_payloads_and_build_ids` |
| Fixed fields, hash, zero padding, unchanged payload integrity | Every bit in offsets 0..63 and 96..259: **1824** separate mutations rejected by both consumers | `every_fixed_header_hash_reserved_and_payload_bit` |
| No truncation, surplus bytes, forged length or transfer oversize | All 260 strict prefixes; 1/255/512-byte extension; size words 0,3,5,UINT32_MAX-255,UINT32_MAX; MAX_IMAGE+1 | `truncation_extension_and_bad_size_words` |
| Bounded printable/NUL-padded build ID, not authentication | Empty, 31 spaces/tildes and valid edit accepted; missing NUL, data after NUL, control/DEL/non-ASCII rejected; producer rejects invalid ID/payload bounds | `build_id_grammar_not_authentication` |
| SHA catches targeted random payload corruption | 256 seeded independently packed payloads with one nonzero byte mutation | `seeded_targeted_corruption` |

Byte-alignment/ARM instruction validity is not asserted: existing consumers and
`test_m0.py:test_short_final_and_minimum_payload` support odd-length payloads.
The prose's reference to aligned ARM code is not an implemented byte-length rule.
Build ID changes that remain grammatical are explicitly accepted.

## Combined integration verification

The combined fresh-main suite passes raw `make test`: 197 tests, 196 passed,
one existing Arm GNU toolchain skip, no failures/errors, no adapters. Earlier
Linux compiler/linker and FAT-label findings below are historical component
results: fixes and regression tests are already on the integration baseline.

`test_large_sparse_geometry_cluster_ceiling_and_card_bounds` extends the fake
card's reported capacity using `set_media_sectors`, restoring its default in
`finally`. It sparsely relocates the generated FAT copies/root/control records,
accepts exactly `0x0fffffed` data clusters, rejects `0x0fffffee`, rejects a card
one sector shorter than the partition, and tests UINT32_MAX subtraction bounds.
Mounts still require zero writes and bounded reads. This is restricted mount
validation, not full-volume fsck or physical-media qualification.

## Original component reproduction and measured results

No external devices, network or third-party Python packages are needed. Standard
harness discovery registers these new files; no shared registration is edited.

```sh
python3 -m unittest discover -s tests -p 'test_m0_*properties.py' -v
make test
```

Campaigns use fixed local `random.Random` instances. Failing subtests include
seed/iteration or exact byte/bit/field; rerun the same test method to replay.

| Campaign | Seed | Iterations |
| --- | --- | ---: |
| Invalid spc | `0x46415432` | 128 |
| Valid fragmented control mapping | `0x4641343f` | 32 |
| Manifest valid payloads/build IDs | `0x534b4931` | 138 (10 fixed + 128 random) |
| Manifest corruption | `0x534b429c` | 256 |
| Fixed manifest/hash/reserved/payload bit campaign | none | 1824 |

On Linux/GCC 15, the unmodified harness fails compilation under `-Werror` due to
existing misleading indentation (`protocol.c:143`, `assets.c:66,117`). An
**environment-only**, uncommitted `cc` adapter used for verification executes:

```sh
exec /usr/bin/cc -Wno-misleading-indentation -Wl,-Bsymbolic "$@"
```

The second flag is required by existing ELF symbol interposition: without it,
M0's `error(const char *)` call resolves to glibc's incompatible `error` function
and crashes in the existing bad-manifest test (confirmed native stack trace).
With this adapter the new 18-test suite passes; the existing M0 suite passes
14 tests. Full `make test` runs 130 tests with four existing checker failures
and one existing Arm-toolchain skip; a pristine baseline runs 112 tests with
the same four failures and one skip. These flags do not disable the
new assertions or alter FAT/manifest production logic.

The full suite additionally requires macOS `fsck_msdos`, absent on this host.
Using a transparent executable adapter to the **real** `/usr/sbin/fsck.fat "$@"`
gets through those calls but yields four existing M1 checker failures: the BPB
has label `SANDBOX M0` while the root directory has no volume-label entry.
The checker returns 1 and says it would remove the label; `-n` leaves media
unchanged. This is reported, not hidden by a fake success or changed assertion.
The Arm GNU toolchain test is separately skipped by its existing condition.
No claim of an unqualified green `make test` or board qualification is made.

Six disposable source mutants were each killed by assertion failures, not build
or import errors: remove MBR signature guard; ignore FAT-copy disagreement;
reduce control spacing threshold to one; ignore C manifest padding; change packer
ABI; ignore C payload SHA comparison. Original sources remain unmodified. Raw
logs and structured mutation results are supplied with the handoff, not committed
injected faults.

## Explicit remaining scope

* Upper/lower cluster and card bounds are now covered by the combined sparse
  integration test above. The existing BPB geometry sum guard is
  unreachable under the accepted count/reserved/FAT bounds: reserved <=65535,
  FAT <=count/4 and count >=262144 imply reserved+2*FAT < count. No fake claim of
  branch coverage for that guard is made.
* SD-init failures, every FAT/directory I/O fault location, every possible
  directory alias/LFN and image-builder full-directory/full-volume failure are
  not exhaustively covered. Metadata generation ordering/CRC/sequence checks
  are covered by the companion control-record suite.
* JSON is producer inventory only; arbitrary JSON validation is not invented.
  CLI operating-system failures and physical USB behavior remain outside these
  host tests. Sender/receiver framing is covered in `test_m0_usb_properties.py`.
* Combined verification and NOTICE attribution are recorded in
  [M0_HOST_FUZZ.md](M0_HOST_FUZZ.md). Publication status belongs in the external
  delivery report, not a claim inferred from these local tests.
