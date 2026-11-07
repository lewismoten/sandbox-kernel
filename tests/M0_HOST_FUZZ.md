# M0 deterministic host fuzz/property tests

Integration base: `a17854357950eec18f7f25c8b19a778c5a41b1f5` (`main`).
No production source changes are required by this topic. Tests use the existing
stdlib unittest discovery, compiled real C core, fake CDC and sparse file-backed
media. They do not establish ARM execution, physical USB timing or power-loss
qualification.

## Added tests

| Module | Tests | Main evidence |
| --- | ---: | --- |
| `test_m0_control_properties.py` | 17 | Wire-field oracle; two valid records each receive all 4096 single-bit flips; repaired-CRC semantic violations; seeded valid/raw/repaired mutations; unsigned selection, conflict, alternating commit and wrap boundaries; separate slot-hash checks |
| `test_m0_fat_properties.py` | 13 | Generated image inventory; MBR/BPB fields and bounds; exact 65524/65525 minimum and `0x0fffffed`/`0x0fffffee` maximum cluster boundary; root loops/hidden overlap; FAT-copy agreement; file extents; disjoint control spacing 1..9; fragmented mappings |
| `test_m0_image_properties.py` | 6 | Literal independent manifest fixture; producer-field oracle; 1824 single-bit header/hash/reserved/payload mutations; truncation/extension/oversize; ID grammar; seeded corrupt payloads |
| `test_m0_usb_properties.py` | 12 | Independent stop-and-wait model plus actual sender/C receiver bridge; fragmented CDC; invalid-image preflight; malformed sizes, truncated blocks, CRC/SHA corruption, surplus data, reply failures and unknown final outcomes |
| **Total** | **48** | Fixed seeds and field/bit/iteration subtest diagnostics |

Detailed control and FAT/image invariant mappings are in
[M0_CONTROL_COVERAGE.md](M0_CONTROL_COVERAGE.md) and
[M0_FAT_IMAGE_COVERAGE.md](M0_FAT_IMAGE_COVERAGE.md).

## USB framing oracle and coverage

`ReceiverModel` defines a separate PUT2 grammar, 64..2097152 transport bounds,
1024-byte stop-and-wait blocks, cumulative ACKs and final VERIFY. Its manifest
oracle uses literal wire constants/offsets and stdlib CRC32/SHA-256, not
`image.validate` or the kernel validator. Valid sender bytes must match exactly;
invalid CRC and content hash cases must agree with the real C parser's final
rejection. Grammatical build IDs remain descriptions, not authentication.

| Property | Witnesses / method suffix |
| --- | --- |
| Sender and real C receiver agree end to end | `sender_real_receiver_{minimum,exact_block,short_final,multiple_blocks}`: total sizes 260,1024,1025,3072; each send is fragmented into seeded 1..197-byte CDC chunks; exact replies and pending record hash/generation checked |
| Sender block boundaries and payload preservation | `sender_model_seeded_block_boundaries`: seed `0x55534250`, eight fixed lengths including maximum plus 128 random lengths; complete received data, no extra replies, exactly one completed transaction |
| Validate before any transport I/O | `sender_rejects_invalid_images_before_any_io`: seven strict prefixes, hash/payload mutation, surplus byte and maximum+1; no INFO/PUT2/payload emitted |
| Truncated block cancellation | `truncated_blocks_timeout_without_writes`: ten prefix cuts including 0,1023,1024,1025,2047,2048; no premature ACK; 15000001 microsecond inactivity yields ERR timeout; no writes/pending commit; next INFO works |
| Oversize and integer overflow | `oversize_and_overflow_commands_never_ready`: below-minimum, maximum+1, UINT32_MAX, UINT32_MAX+1 and 31-digit size; reject without READY or writes |
| Wrong transport CRC and manifest/payload SHA | `bad_crc_and_hashes_against_model`: seed `0x55534248`, 96 cases split among CRC, manifest-hash and payload corruption; repaired transport CRC for SHA witnesses; each real-C rejection equals model and preserves confirmed slot/control |
| Extra bytes | `surplus_payload_rejected_before_commit`: trailing input after VERIFY rejects before media writes |
| Stop on failed flow control | `sender_stops_on_wrong_ready_ack_or_verify`: injected READY/ACK/VERIFY errors; exact sent block count and one PUT2, no blind retry |
| Lost final reply | `sender_lost_final_is_unknown_without_resend`: TimeoutError and OSError after VERIFY return 2, identify unknown outcome, prohibit blind resend and emit exactly one PUT2 |

Existing `test_m0.py` additionally executes the real C receiver's maximum-size
transfer. Existing `test_host.py` covers initial INFO retries and delayed INFO.
The independent model intentionally does not claim to model electrical USB,
TinyUSB queues, SD failure physics or the ARM selector's hardware entry.

## Verification

```sh
make test
python3 -m unittest discover -s tests -p 'test_m0_*properties.py' -v
```

Measured combined result: **197 tests, 196 passed, one existing skip**, no
failures/errors; the skip is `test_abi.Layout.test_arm32_layout_and_svc` because
the Arm GNU toolchain is not installed. No compiler/checker environment adapters
are needed on the integration baseline. All 48 new tests also pass in reverse
order. The variable fake-card capacity is restored in `finally`; other suites
retain the original 264192-sector default.

Eight additional disposable source mutants were detected by assertions, not
compiler/import errors: bypass sender preflight, shorten sender blocks, ignore
reply mismatch, omit receive CRC, omit receive manifest/SHA validation, omit
receive timeout, omit maximum transport size, and relax maximum cluster count.
The component suites independently detected 12 control and six FAT/image guard
mutants. Mutants are not part of the topic's source tree.

No new production defect was demonstrated in this combined run. Earlier host
symbol-binding/compiler and missing FAT root-volume-label findings already have
separate fixes/regressions on the integration baseline; this topic does not
reapply them. NOTICE.md records the actual GPT-6.1 Sol / Hermes Agent runtime.
