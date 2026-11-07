# SPDX-License-Identifier: MIT
"""M0 image producer/consumer contract, independent wire fixtures and mutations."""
import ctypes as C
import hashlib
import pathlib
import random
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import image
import hostlib

SEED = 0x534B4931


class ImageProperties(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.c = C.CDLL(str(hostlib.build(pathlib.Path(cls.temp.name) / 'image.so')))
        cls.c.image_valid.argtypes = [C.c_void_p, C.c_uint32]
        cls.c.image_valid.restype = C.c_bool

    def accepted(self, wire):
        self.assertEqual(image.validate(wire), wire)
        self.assertTrue(self.c.image_valid(wire, len(wire)))

    def rejected(self, wire):
        with self.assertRaises(ValueError):
            image.validate(wire)
        self.assertFalse(self.c.image_valid(wire, len(wire)))

    @staticmethod
    def golden():
        # Literal v1 header and published SHA-256("abcd"), not image.pack output.
        return bytes.fromhex(
            '534b493101000000010000000000010004000000000010000000100000000000'
            '88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589') + b'gold' + bytes(188) + b'abcd'

    def test_literal_wire_and_producer(self):
        wire = self.golden()
        self.assertEqual(len(wire), 260)
        self.accepted(wire)
        self.assertEqual(image.pack(b'abcd', 'gold'), wire)

    def test_seeded_payloads_and_build_ids(self):
        rng = random.Random(SEED)
        lengths = [4, 5, 255, 256, 257, 511, 512, 768, 769, image.MAX_IMAGE - 256]
        lengths += [rng.randrange(4, 8193) for _ in range(128)]
        for iteration, n in enumerate(lengths):
            with self.subTest(seed=SEED, iteration=iteration, bytes=n):
                payload = rng.randbytes(n)
                build = ''.join(chr(rng.randrange(32, 127)) for _ in range(iteration % 32))
                wire = image.pack(payload, build)
                self.assertEqual(wire[:32], struct.pack('<8I', 0x31494B53, 1, 1, 0x10000, n, 0x100000, 0x100000, 0))
                self.assertEqual(wire[32:64], hashlib.sha256(payload).digest())
                self.assertEqual(wire[64:96], build.encode('ascii').ljust(32, b'\0'))
                self.assertEqual(wire[96:256], bytes(160))
                self.assertEqual(wire[256:], payload)
                self.accepted(wire)

    def test_every_fixed_header_hash_reserved_and_payload_bit(self):
        wire = self.golden()
        # Build ID is descriptive, not hashed: its valid edits are tested separately.
        for offset in list(range(64)) + list(range(96, 260)):
            for bit in range(8):
                with self.subTest(offset=offset, bit=bit):
                    bad = bytearray(wire)
                    bad[offset] ^= 1 << bit
                    self.rejected(bytes(bad))

    def test_truncation_extension_and_bad_size_words(self):
        wire = self.golden()
        for n in range(260):
            with self.subTest(truncated=n):
                self.rejected(wire[:n])
        for n in (1, 255, 512):
            self.rejected(wire + bytes(n))
        for n in (0, 3, 5, 0xFFFFFF00, 0xFFFFFFFF):
            bad = bytearray(wire)
            struct.pack_into('<I', bad, 16, n)
            self.rejected(bytes(bad))
        self.rejected(wire + bytes(image.MAX_IMAGE + 1 - len(wire)))

    def test_build_id_grammar_not_authentication(self):
        wire = self.golden()
        for build in (b'\0' * 32, b' ' * 31 + b'\0', b'~' * 31 + b'\0', b'edited' + bytes(26)):
            self.accepted(wire[:64] + build + wire[96:])
        for build in (b'A' * 32, b'A\0B' + bytes(29), b'\x1f' + bytes(31), b'\x7f' + bytes(31), b'\xff' + bytes(31)):
            self.rejected(wire[:64] + build + wire[96:])
        for build in ('x' * 32, 'bad\0id', '\x1f', '\x7f', 'é'):
            with self.subTest(build=repr(build)), self.assertRaises(ValueError):
                image.pack(b'abcd', build)
        for n in (0, 1, 2, 3, image.MAX_IMAGE - 255):
            with self.subTest(payload=n), self.assertRaises(ValueError):
                image.pack(bytes(n), 'bounded')

    def test_seeded_targeted_corruption(self):
        rng = random.Random(SEED ^ 0xBAD)
        for iteration in range(256):
            payload = rng.randbytes(rng.randrange(4, 4096))
            wire = bytearray(image.pack(payload, 'mutation'))
            # Always corrupt at least one payload byte without rehashing.
            offset = rng.randrange(256, len(wire))
            wire[offset] ^= rng.randrange(1, 256)
            with self.subTest(seed=SEED ^ 0xBAD, iteration=iteration, offset=offset):
                self.rejected(bytes(wire))


if __name__ == '__main__':
    unittest.main()
