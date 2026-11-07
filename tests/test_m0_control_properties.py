# SPDX-License-Identifier: MIT
"""Control-record properties through existing public host storage interfaces.

The independent wire oracle below states record-local invariants, with numeric
policies not fully specified in docs/m0.md identified in M0_CONTROL_COVERAGE.md.
Ordering and transitions are separate store_init/commit properties. Randomized
failure messages include the complete 512-byte input, seed and iteration.
"""
import ctypes as C
import hashlib
import json
import os
import pathlib
import random
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tests'))
import hostlib
import image

MAGIC = 0x31434B53
NONE = 0xFFFFFFFF
SEED = 0x4D304354
WORDS = struct.Struct('<128I')


def seal(record):
    out = bytearray(record)
    struct.pack_into('<I', out, 508, zlib.crc32(out[:508]))
    return bytes(out)


def change(record, fields, repair=True):
    out = bytearray(record)
    for word, value in fields.items():
        struct.pack_into('<I', out, word * 4, value)
    return seal(out) if repair else bytes(out)


def record(sequence=1, slot=0, generation=1, pending=NONE,
           pending_generation=0, consumed=0, confirmed_hash=bytes(range(32)),
           pending_hash=bytes(32)):
    out = bytearray(512)
    struct.pack_into('<9I', out, 0, MAGIC, 1, sequence, slot, pending,
                     consumed, generation, pending_generation, NONE)
    out[64:96] = confirmed_hash
    out[96:128] = pending_hash
    return seal(out)


def wire_valid(data):
    """No production constants/helpers or C validator used as the oracle.

    Generations are ordinary unsigned numbers, not serial-number arithmetic.
    A maximum generation is locally representable; wrapping transitions are not.
    Hash content is opaque here; slot_load checks its meaning against slot bytes.
    """
    if len(data) != 512:
        return False
    w = WORDS.unpack(data)
    if data[:8] != b'SKC1\x01\0\0\0' or not 1 <= w[2] <= 0xFFFFFFFE:
        return False
    if w[3] not in (0, 1) or w[4] not in (0, 1, NONE):
        return False
    if w[5] not in (0, 1) or w[6] == 0 or w[8] != NONE:
        return False
    if any(data[36:64]) or any(data[128:508]):
        return False
    if w[4] == NONE:
        if w[5] != 0 or w[7] != 0 or any(data[96:128]):
            return False
    elif w[4] == w[3] or w[7] <= w[6]:
        return False
    return w[127] == zlib.crc32(data[:508])


class ControlProperties(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        cls.library = hostlib.build(cls.work / 'control.so')
        cls.known = image.pack(bytes(range(256)) * 4, 'control-property')
        cls.digest = hashlib.sha256(cls.known).digest()
        cls.initial = record(confirmed_hash=cls.digest)
        folder = cls.work / 'boot'
        folder.mkdir()
        for name in ('KERNA.BIN', 'KERNB.BIN'):
            (folder / name).write_bytes(cls.known.ljust(image.MAX_IMAGE, b'\0'))
        for n in range(2):
            (folder / f'BOOTCT{n}.BIN').write_bytes(cls.initial + bytes(3584))
        cls.media = cls.work / 'card.img'
        image.fat_image(folder, cls.media)
        cls.layout = json.loads(cls.media.with_suffix('.layout.json').read_text())['files']
        cls.offsets = [cls.layout[f'BOOTCT{n}.BIN']['first_lba'] * 512 for n in range(2)]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        lib = pathlib.Path(self.tmp.name) / 'isolated.so'
        shutil.copyfile(self.library, lib)
        self.c = C.CDLL(str(lib))
        self.c.open_disk.argtypes = [C.c_char_p]
        for name in ('store_init', 'control_healthy', 'control_commit', 'initialize', 'slot_load'):
            getattr(self.c, name).restype = C.c_bool
        self.c.control_commit.argtypes = [C.POINTER(C.c_uint32)]
        self.c.slot_load.argtypes = [C.c_uint, C.c_void_p, C.POINTER(C.c_uint32)]
        self.c.write_count.restype = C.c_uint32
        self.c.take_output.restype = C.c_char_p
        self.control = (C.c_uint32 * 128).in_dll(self.c, 'control')
        self.opened = False
        self.mount(self.initial, self.initial)
        self.assertEqual(self.c.write_count(), 0)

    def tearDown(self):
        self.close()
        self.tmp.cleanup()

    def close(self):
        if self.opened:
            self.c.close_disk()
            self.opened = False

    def put(self, copies):
        # Close stdio before external writes: no stale C FILE buffers.
        self.close()
        with self.media.open('r+b') as out:
            for index, data in copies.items():
                out.seek(self.offsets[index])
                out.write(data)
        self.c.open_disk(os.fsencode(self.media))
        self.opened = True

    def mount(self, left, right):
        self.put({0: left, 1: right})
        return bool(self.c.store_init())

    def selected(self):
        return bytes(self.control)

    def healthy(self, candidate):
        # setUp selected copy 0; no commit occurs in record-local campaigns.
        # Matching disk/RAM isolates validity from control_healthy's equality guard.
        self.put({0: candidate})
        C.memmove(self.control, candidate, 512)
        return bool(self.c.control_healthy())

    def check_wire(self, candidate, expected=None, context=''):
        wanted = wire_valid(candidate) if expected is None else expected
        self.assertEqual(wire_valid(candidate), wanted, 'oracle fixture: ' + context)
        self.assertEqual(self.healthy(candidate), wanted,
                         f'{context} input={candidate.hex()}')

    def test_independent_fixture_matches_packer_and_crc_vector(self):
        self.assertEqual(self.initial, image.control_record(self.known))
        self.assertEqual(zlib.crc32(b'123456789'), 0xCBF43926)
        self.c.crc32.argtypes = [C.c_void_p, C.c_uint32]
        self.c.crc32.restype = C.c_uint32
        self.assertEqual(self.c.crc32(b'123456789', 9), 0xCBF43926)
        self.check_wire(self.initial, True, 'initial')

    def test_every_single_bit_flip_without_crc_repair(self):
        fixtures = [self.initial, record(sequence=0xFFFFFFFE, slot=1,
                    generation=0x7FFFFFFF, pending=0, pending_generation=0xFFFFFFFE,
                    consumed=1, confirmed_hash=self.digest, pending_hash=self.digest)]
        for fixture_index, valid in enumerate(fixtures):
            self.check_wire(valid, True, f'fixture={fixture_index}')
            for bit in range(4096):
                bad = bytearray(valid)
                bad[bit // 8] ^= 1 << (bit % 8)
                self.check_wire(bytes(bad), False, f'fixture={fixture_index} bit={bit}')
        self.assertEqual(self.c.write_count(), 0)

    def test_semantic_invalid_fields_with_repaired_crc(self):
        cases = {
            0: [0, MAGIC ^ 1, NONE], 1: [0, 2, NONE], 2: [0, NONE],
            3: [2, 0x80000000, NONE], 4: [0, 2, 0xFFFFFFFE],
            5: [1, 2, NONE], 6: [0], 7: [1, NONE], 8: [0, 1, 0xFFFFFFFE],
        }
        for word, values in cases.items():
            for value in values:
                self.check_wire(change(self.initial, {word: value}), False,
                                f'field word={word} value={value:#x}')
        # Every reserved word and every absent pending-hash word, not just endpoints.
        for word in list(range(9, 16)) + list(range(24, 32)) + list(range(32, 127)):
            for value in (1, 0x80000000, NONE):
                self.check_wire(change(self.initial, {word: value}), False,
                                f'reserved word={word} value={value:#x}')
        for slot in (0, 1):
            pending = record(slot=slot, pending=1-slot, generation=0x80000000,
                             pending_generation=0x80000001, pending_hash=self.digest)
            for gen in (0, 1, 0x7FFFFFFF, 0x80000000):
                self.check_wire(change(pending, {7: gen}), False,
                                f'pending slot={slot} generation={gen:#x}')
            self.check_wire(change(pending, {4: slot}), False, 'same slot pending')
            self.check_wire(change(pending, {5: 2}), False, 'attempt overflow')
        self.assertEqual(self.c.write_count(), 0)

    def test_valid_boundary_combinations_and_opaque_hashes(self):
        for seq in (1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE):
            for gen in (1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, NONE):
                for slot in (0, 1):
                    self.check_wire(record(seq, slot, gen), True,
                                    f'absent seq={seq:#x} gen={gen:#x} slot={slot}')
                    if gen < NONE:
                        for consumed in (0, 1):
                            self.check_wire(record(seq, slot, gen, 1-slot, gen+1,
                                            consumed, bytes(32), bytes(32)), True,
                                            f'pending seq={seq:#x} gen={gen:#x} slot={slot} consumed={consumed}')

    def test_seeded_valid_records(self):
        rng = random.Random(SEED)
        for iteration in range(1024):
            gen = rng.randint(1, 0xFFFFFFFE)
            slot = rng.randrange(2)
            pending = rng.choice((False, True))
            candidate = record(rng.randint(1, 0xFFFFFFFE), slot, gen,
                               1-slot if pending else NONE,
                               rng.randint(gen+1, NONE) if pending else 0,
                               rng.randrange(2) if pending else 0,
                               rng.randbytes(32), rng.randbytes(32) if pending else bytes(32))
            self.check_wire(candidate, True, f'seed={SEED:#x} iteration={iteration}')

    def test_seeded_mutations_with_and_without_crc_repair(self):
        # Include valid-preserving hash/sequence changes as well as invalid fields.
        for repair in (False, True):
            seed = SEED ^ (0x100 if repair else 0x200)
            rng = random.Random(seed)
            valid = record(pending=1, pending_generation=2, pending_hash=self.digest)
            accepted = rejected = 0
            for iteration in range(2048):
                candidate = bytearray(valid if iteration % 2 else self.initial)
                for _ in range(rng.randint(1, 5)):
                    at = rng.randrange(512)
                    candidate[at] ^= rng.randint(1, 255)
                if repair:
                    candidate = seal(candidate)
                candidate = bytes(candidate)
                expected = wire_valid(candidate)
                accepted += expected
                rejected += not expected
                self.check_wire(candidate, context=f'seed={seed:#x} iteration={iteration} repair={repair}')
            self.assertGreater(rejected, 0)
            if repair:
                self.assertGreater(accepted, 0, 'campaign must include accepted mutations')

    def test_healthy_requires_exact_disk_ram_identity(self):
        other = change(self.initial, {2: 2})
        self.put({0: other})
        self.assertTrue(wire_valid(other))
        self.assertFalse(self.c.control_healthy())
        self.assertEqual(self.selected(), self.initial)
        self.assertEqual(self.c.write_count(), 0)

    def test_mount_rejects_invalid_copy_and_recovers_without_metadata(self):
        for word, value in ((0, 0), (1, 2), (2, 0), (3, 2), (4, 0),
                            (5, 1), (6, 0), (7, 1), (8, 0), (9, 1), (24, 1), (126, 1)):
            bad = change(self.initial, {word: value})
            for invalid_index in (0, 1):
                copies = [self.initial, self.initial]
                copies[invalid_index] = bad
                with self.subTest(word=word, value=value, invalid_index=invalid_index):
                    self.assertTrue(self.mount(*copies))
                    self.assertEqual(self.selected(), self.initial)
            self.assertTrue(self.mount(bad, bad))
            self.assertEqual(self.selected(), bytes(512), f'word={word}')
            next_record = (C.c_uint32 * 128).from_buffer_copy(self.initial)
            self.assertFalse(self.c.control_commit(next_record))
        self.assertEqual(self.c.write_count(), 0)

    def test_corrupt_newest_copy_falls_back_in_both_positions(self):
        newest = change(self.initial, {2: 100})
        for bit in (0, 64, 512, 768, 4064, 4095):
            bad = bytearray(newest)
            bad[bit // 8] ^= 1 << (bit % 8)
            for invalid_index in (0, 1):
                copies = [self.initial, self.initial]
                copies[invalid_index] = bytes(bad)
                self.assertTrue(self.mount(*copies), f'bit={bit} copy={invalid_index}')
                self.assertEqual(self.selected(), self.initial,
                                 f'bit={bit} copy={invalid_index} input={bytes(bad).hex()}')
        self.assertEqual(self.c.write_count(), 0)

    def test_failed_commit_preserves_selected_record_and_media(self):
        proposed = (C.c_uint32 * 128).from_buffer_copy(
            record(pending=1, pending_generation=2, confirmed_hash=self.digest,
                   pending_hash=self.digest))
        self.c.inject_failure(0)
        self.assertFalse(self.c.control_commit(proposed))
        self.assertEqual(self.selected(), self.initial)
        self.assertTrue(self.c.control_healthy())
        self.close()
        with self.media.open('rb') as media:
            for offset in self.offsets:
                media.seek(offset)
                self.assertEqual(media.read(512), self.initial)
        self.assertEqual(self.c.write_count(), 1)  # harness counts failed attempts

    def test_sequence_selection_unsigned_order_and_equal_conflicts(self):
        sequences = (1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFD, 0xFFFFFFFE)
        for left_seq in sequences:
            for right_seq in sequences:
                left = record(left_seq, confirmed_hash=self.digest)
                right = record(right_seq, confirmed_hash=self.digest)
                self.assertTrue(self.mount(left, right), f'left={left_seq:#x} right={right_seq:#x}')
                self.assertEqual(self.selected(), left if left_seq >= right_seq else right)
        # Equal sequence is allowed only for byte-identical copies, even if both
        # candidate records are individually semantically valid and checksummed.
        for word in (3, 6, 16, 23):
            other = change(self.initial, {word: 1 if word == 3 else 2})
            self.assertTrue(wire_valid(other))
            for pair in ((self.initial, other), (other, self.initial)):
                self.assertFalse(self.mount(*pair), f'conflict word={word}')
                self.assertEqual(C.c_char_p.in_dll(self.c, 'store_fail').value, b'CTRL CONFLICT')
                self.assertEqual(self.selected(), bytes(512))
        self.assertEqual(self.c.write_count(), 0)

    def test_seeded_selection_uses_sequence_not_generation(self):
        seed = SEED ^ 0x300
        rng = random.Random(seed)
        for iteration in range(128):
            # Independently valid copies may contain a much older generation in
            # the newer sequence. There is no documented cross-copy gen ordering.
            a, b = rng.sample(range(1, 0xFFFFFFFF), 2)
            left = record(a, generation=rng.randint(1, NONE), confirmed_hash=self.digest)
            right = record(b, generation=rng.randint(1, NONE), confirmed_hash=self.digest)
            context = f'seed={seed:#x} iteration={iteration} left={left.hex()} right={right.hex()}'
            self.assertTrue(self.mount(left, right), context)
            self.assertEqual(self.selected(), left if a > b else right, context)
        self.assertEqual(self.c.write_count(), 0)

    def test_commit_alternates_older_copy_and_preserves_previous_record(self):
        for start in (0, 1):
            newest = change(self.initial, {2: 10})
            pair = [self.initial, self.initial]
            pair[start] = newest
            self.assertTrue(self.mount(*pair))
            for step in range(4):
                before = self.selected()
                # Caller sequence and CRC are intentionally irrelevant: commit
                # owns both. Pending generation uses ordinary unsigned ordering.
                candidate = change(before, {2: NONE, 4: 1, 5: step % 2, 7: 2}, repair=False)
                proposed = (C.c_uint32 * 128).from_buffer_copy(candidate)
                self.assertTrue(self.c.control_commit(proposed))
                after = self.selected()
                self.assertEqual(after, change(candidate, {2: 11 + step}))
                target = 1 - (start if step % 2 == 0 else 1-start)
                self.close()
                with self.media.open('rb') as media:
                    media.seek(self.offsets[target]); self.assertEqual(media.read(512), after)
                    media.seek(self.offsets[1-target]); self.assertEqual(media.read(512), before)
                self.c.open_disk(os.fsencode(self.media)); self.opened = True
                self.assertTrue(self.c.control_healthy())
        self.assertEqual(self.c.write_count(), 8)

    def test_sequence_wrap_transition_refuses_writes(self):
        near_end = change(self.initial, {2: 0xFFFFFFFD})
        self.assertTrue(self.mount(near_end, near_end))
        proposed = (C.c_uint32 * 128).from_buffer_copy(near_end)
        self.assertTrue(self.c.control_commit(proposed))
        self.assertEqual(self.control[2], 0xFFFFFFFE)
        before = self.selected()
        count = self.c.write_count()
        self.assertFalse(self.c.control_commit(proposed))
        self.assertEqual(self.selected(), before)
        self.assertEqual(self.c.write_count(), count)
        for seq in (0, NONE):
            invalid = change(self.initial, {2: seq})
            self.assertTrue(self.mount(invalid, before))
            self.assertEqual(self.selected(), before, f'seq={seq:#x} must not wrap to newest')

    def test_commit_rejects_semantic_invalidity_without_writes(self):
        for fields in ({0: 0}, {1: 2}, {3: 2}, {4: 0}, {5: 1}, {6: 0},
                       {7: 2}, {8: 0}, {9: 1}, {24: 1}, {126: 1},
                       {4: 1, 7: 1}, {4: 1, 7: 0}, {4: 1, 7: 2, 5: 2}):
            candidate = (C.c_uint32 * 128).from_buffer_copy(change(self.initial, fields))
            self.assertFalse(self.c.control_commit(candidate), f'fields={fields}')
            self.assertEqual(self.selected(), self.initial)
        self.assertEqual(self.c.write_count(), 0)

    def test_generation_unsigned_order_and_no_wrap_on_commit(self):
        for confirmed, pending, expected in (
                (1, 2, True), (0x7FFFFFFF, 0x80000000, True),
                (0x80000000, 0x7FFFFFFF, False), (0xFFFFFFFE, NONE, True),
                (NONE, 0, False), (NONE, 1, False), (NONE, NONE, False)):
            base = record(generation=confirmed, confirmed_hash=self.digest)
            self.assertTrue(self.mount(base, base))
            proposed = record(generation=confirmed, pending=1,
                              pending_generation=pending, confirmed_hash=self.digest,
                              pending_hash=self.digest)
            before_writes = self.c.write_count()
            self.assertEqual(self.c.control_commit((C.c_uint32 * 128).from_buffer_copy(proposed)), expected,
                             f'confirmed={confirmed:#x} pending={pending:#x} input={proposed.hex()}')
            self.assertEqual(self.c.write_count() - before_writes, int(expected))
            if not expected:
                self.assertEqual(self.selected(), base)

    def test_slot_hash_is_checked_separately_from_record_validity(self):
        wrong_hash = change(self.initial, {16: 0})
        self.assertTrue(self.mount(wrong_hash, wrong_hash))
        self.assertTrue(self.c.control_healthy())
        dest = C.create_string_buffer(image.MAX_IMAGE)
        size = C.c_uint32()
        self.assertFalse(self.c.slot_load(0, dest, C.byref(size)))
        self.assertTrue(self.mount(self.initial, self.initial))
        self.assertTrue(self.c.slot_load(0, dest, C.byref(size)))
        self.assertEqual(dest.raw[:size.value], self.known)
        self.assertEqual(self.c.write_count(), 0)


if __name__ == '__main__':
    unittest.main()
