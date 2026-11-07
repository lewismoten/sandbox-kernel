# SPDX-License-Identifier: MIT
"""Seeded, reversible mutations of image.py media through the real store_init."""
import contextlib
import ctypes as C
import hashlib
import json
import os
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

SEED = 0x46415432
BASE, FAT, FAT_SIZE, DATA = 2048, 2080, 2048, 6176


class FatProperties(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        work = pathlib.Path(cls.temp.name)
        cls.c = C.CDLL(str(hostlib.build(work / 'fat.so')))
        cls.c.open_disk.argtypes = [C.c_char_p]
        cls.c.store_init.restype = C.c_bool
        cls.c.read_count.restype = C.c_uint32
        cls.c.write_count.restype = C.c_uint32
        cls.boot = work / 'boot'
        cls.boot.mkdir()
        wire = image.pack(b'abcd', 'fat-fixture')
        cls.record = image.control_record(wire)
        for name, contents in {
            'KERNA.BIN': wire.ljust(image.MAX_IMAGE, b'\0'),
            'KERNB.BIN': bytes(image.MAX_IMAGE),
            'BOOTCT0.BIN': cls.record + bytes(3584),
            'BOOTCT1.BIN': cls.record + bytes(3584),
        }.items():
            (cls.boot / name).write_bytes(contents)
        cls.disk = work / 'card.img'
        image.fat_image(cls.boot, cls.disk)
        cls.layout = json.loads(cls.disk.with_suffix('.layout.json').read_text())
        with cls.disk.open('rb') as f:
            f.seek(DATA * 512)
            cls.root = f.read(1024)
        cls.entries = {name: cls.root.index(image.short_name(name)) for name in cls.layout['files']}
        cls.starts = {name: (v['first_lba'] - DATA + 2) for name, v in cls.layout['files'].items()}

    @contextlib.contextmanager
    def media(self):
        # Journal only touched byte ranges; reverse restoration supports overlaps.
        undo = []
        def patch(offset, data):
            with self.disk.open('r+b') as f:
                f.seek(offset)
                old = f.read(len(data))
                self.assertEqual(len(old), len(data))
                undo.append((offset, old))
                f.seek(offset)
                f.write(data)
        try:
            yield patch
        finally:
            with self.disk.open('r+b') as f:
                for offset, old in reversed(undo):
                    f.seek(offset)
                    f.write(old)

    def probe(self, accepted=True, reason='none', max_reads=20000, disk=None):
        self.c.open_disk(os.fsencode(self.disk if disk is None else disk))
        before = self.c.read_count()
        try:
            self.assertEqual(self.c.store_init(), accepted)
            self.assertEqual(C.c_char_p.in_dll(self.c, 'store_fail').value.decode(), reason)
            self.assertLessEqual(self.c.read_count() - before, max_reads)
            self.assertEqual(self.c.write_count(), 0, 'mount must never write media')
        finally:
            self.c.close_disk()

    def fat_entry(self, patch, cluster, value, copies=(0, 1)):
        for copy in copies:
            patch((FAT + copy * FAT_SIZE) * 512 + cluster * 4, struct.pack('<I', value))

    def dir_start(self, patch, name, cluster):
        at = DATA * 512 + self.entries[name]
        patch(at + 20, struct.pack('<H', cluster >> 16))
        patch(at + 26, struct.pack('<H', cluster & 65535))

    def test_generated_layout_inventory_and_read_only_mount(self):
        self.assertEqual(self.layout['format'], 1)
        self.assertEqual(self.layout['sector_bytes'], 512)
        self.assertEqual(self.disk.stat().st_size, 135266304)
        self.assertEqual((self.layout['partition_start'], self.layout['partition_sectors']), (2048, 262144))
        with self.disk.open('rb') as f:
            self.assertEqual(f.read(512)[510:], b'\x55\xaa')
            for name, item in self.layout['files'].items():
                f.seek(item['first_lba'] * 512)
                content = f.read(item['bytes'])
                self.assertEqual(content, (self.boot / name).read_bytes())
                self.assertEqual(hashlib.sha256(content).hexdigest(), item['sha256'])
                self.assertEqual(item['sectors'] * 512, item['bytes'])
                start = item['first_lba'] - DATA + 2
                for copy in (0, 1):
                    f.seek((FAT + copy * FAT_SIZE) * 512 + start * 4)
                    chain = struct.unpack('<' + 'I' * item['sectors'], f.read(item['sectors'] * 4))
                    self.assertEqual(chain[:-1], tuple(range(start + 1, start + item['sectors'])))
                    self.assertGreaterEqual(chain[-1] & 0xFFFFFFF, 0xFFFFFF8)
        self.probe()

    def test_truncated_media_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            short = pathlib.Path(temp) / 'short.img'
            for length, reason in ((0, 'SD READ0'), (511, 'SD READ0'),
                                   (512, 'SD READ BPB'), (BASE * 512 + 511, 'SD READ BPB'),
                                   (DATA * 512 + 511, 'DIR READ')):
                with self.subTest(length=length):
                    with self.disk.open('rb') as src, short.open('wb') as dst:
                        dst.write(src.read(length))
                    self.probe(False, reason, 4, disk=short)

    def test_generated_image_is_deterministic(self):
        with tempfile.TemporaryDirectory() as temp:
            other = pathlib.Path(temp) / 'repeat.img'
            image.fat_image(self.boot, other)
            self.assertEqual(self.disk.stat().st_size, other.stat().st_size)
            self.assertEqual(self.disk.with_suffix('.layout.json').read_bytes(), other.with_suffix('.layout.json').read_bytes())
            with self.disk.open('rb') as first, other.open('rb') as second:
                for offset in range(0, self.disk.stat().st_size, 1024 * 1024):
                    self.assertEqual(first.read(1024 * 1024), second.read(1024 * 1024), offset)
            self.probe(disk=other)

    def test_mbr_and_bpb_boundary_mutations(self):
        # Absolute byte offset, field encoding, invalid value, stable diagnostic.
        cases = [(510, 'H', 0, 'MBR SIG'), (450, 'B', 0x83, 'MBR TYPE')]
        cases += [(454, 'I', v, 'MBR RANGE') for v in (0, 2047, 2049, 264192, 0xFFFFFFFF)]
        cases += [(458, 'I', v, 'MBR RANGE') for v in (0, 262143, 262145, 0xFFFFFFFF)]
        cases += [(BASE * 512 + o, fmt, v, 'BPB') for o, fmt, v in [
            (510, 'H', 0), (11, 'H', 511), (11, 'H', 1024), (16, 'B', 1), (16, 'B', 3),
            (17, 'H', 1), (22, 'H', 1), (42, 'H', 1)]]
        cases += [(BASE * 512 + 13, 'B', v, 'BPB GEOM') for v in (0, 3, 127, 129, 255)]
        cases += [(BASE * 512 + o, fmt, v, 'BPB GEOM') for o, fmt, v in [
            (14, 'H', 0), (36, 'I', 0), (36, 'I', 65537), (36, 'I', 0xFFFFFFFF),
            (40, 'H', 1), (32, 'I', 262143), (32, 'I', 262145)]]
        # FAT too short for data clusters; spc=4 drops below FAT32 cluster minimum.
        cases += [(BASE * 512 + 36, 'I', 2016, 'BPB CLUSTERS')]
        cases += [(BASE * 512 + 13, 'B', v, 'BPB CLUSTERS') for v in (4, 8, 16, 32, 64, 128)]
        for offset, fmt, value, reason in cases:
            with self.subTest(offset=offset, value=value), self.media() as patch:
                patch(offset, struct.pack('<' + fmt, value))
                self.probe(False, reason, 4)

    def test_seeded_malformed_geometry(self):
        rng = random.Random(SEED)
        for iteration in range(128):
            bad_spc = rng.choice([v for v in range(1, 256) if v & (v - 1) or v > 128])
            with self.subTest(seed=SEED, iteration=iteration, spc=bad_spc), self.media() as patch:
                patch(BASE * 512 + 13, bytes([bad_spc]))
                self.probe(False, 'BPB GEOM', 4)

    def test_supported_type_fat_tags_and_two_sector_clusters(self):
        for tag in range(0xFFFFFF8, 0x10000000):
            with self.subTest(eoc=tag), self.media() as patch:
                patch(450, b'\x0b')
                # Reserved FAT high nibble is ignored in both copies.
                self.fat_entry(patch, 3, tag | 0xA0000000)
                self.probe()
        with self.media() as patch:
            patch(BASE * 512 + 13, b'\x02')
            fat = bytearray(FAT_SIZE * 512)
            struct.pack_into('<3I', fat, 0, 0xFFFFFF8, 0xFFFFFFFF, 0xFFFFFFF)
            for name, item in self.layout['files'].items():
                start = (self.starts[name] - 2) // 2 + 2
                count = item['sectors'] // 2
                self.dir_start(patch, name, start)
                for i in range(count):
                    struct.pack_into('<I', fat, (start + i) * 4, start + i + 1 if i + 1 < count else 0xFFFFFFF)
            for copy in (0, 1):
                patch((FAT + copy * FAT_SIZE) * 512, fat)
            self.probe()

    def test_exact_fat32_cluster_minimum(self):
        # A large sparse FAT reaches the exact FAT32 cutoff on the fixed card.
        reserved, fat_size, spc = 22, 65536, 2
        data = BASE + reserved + 2 * fat_size
        clusters = (264192 - data) // spc
        self.assertEqual(clusters, 65525)
        with tempfile.TemporaryDirectory() as temp:
            disk = pathlib.Path(temp) / 'minimum.img'
            with self.disk.open('rb') as src:
                mbr = src.read(512)
                src.seek(BASE * 512)
                boot = bytearray(src.read(512))
                src.seek((BASE + 1) * 512)
                fsinfo = bytearray(src.read(512))
            boot[13] = spc
            struct.pack_into('<H', boot, 14, reserved)
            struct.pack_into('<I', boot, 36, fat_size)
            root = bytearray(self.root)
            fat = bytearray(20480)
            struct.pack_into('<3I', fat, 0, 0xFFFFFF8, 0xFFFFFFFF, 0xFFFFFFF)
            used, next_free = 1, 3
            with disk.open('wb') as out:
                out.truncate(264192 * 512)
                def write(lba, contents):
                    out.seek(lba * 512)
                    out.write(contents)
                for name, item in self.layout['files'].items():
                    start = (self.starts[name] - 2) // 2 + 2
                    count = item['sectors'] // 2
                    used += count
                    next_free = max(next_free, start + count)
                    at = self.entries[name]
                    struct.pack_into('<H', root, at + 20, start >> 16)
                    struct.pack_into('<H', root, at + 26, start & 65535)
                    for i in range(count):
                        struct.pack_into('<I', fat, (start + i) * 4, start + i + 1 if i + 1 < count else 0xFFFFFFF)
                    write(data + (start - 2) * spc, (self.boot / name).read_bytes())
                struct.pack_into('<II', fsinfo, 488, clusters - used, next_free)
                write(0, mbr)
                for lba in (BASE, BASE + 6):
                    write(lba, boot)
                for lba in (BASE + 1, BASE + 7):
                    write(lba, fsinfo)
                write(data, root)
                for lba in (BASE + reserved, BASE + reserved + fat_size):
                    write(lba, fat)
            self.probe(disk=disk)
            # A two-sector reserved-area increase makes exactly 65524 clusters.
            with disk.open('r+b') as out:
                out.seek(BASE * 512 + 14)
                out.write(struct.pack('<H', reserved + 2))
            self.probe(False, 'BPB CLUSTERS', 4, disk=disk)

    def test_root_chain_termination_and_overlap(self):
        for root in (0, 1, 258018, 0xFFFFFFFF):
            with self.subTest(root=root), self.media() as patch:
                patch(BASE * 512 + 44, struct.pack('<I', root))
                self.probe(False, 'DIR CLUSTER', 4)
        with self.media() as patch:
            self.fat_entry(patch, 3, 2)  # loop after the first unused entry
            self.probe(False, 'DIR END2', 17000)
        with self.media() as patch:
            patch(DATA * 512 + len(self.layout['files']) * 32, b'\xe5' * (1024 - len(self.layout['files']) * 32))
            self.fat_entry(patch, 3, 2)
            self.probe(False, 'DIR DUP', 16)  # repeated named entries fail before hop cap
        with self.media() as patch:
            patch(DATA * 512, b'\xe5' * 1024)
            self.fat_entry(patch, 3, 2)
            self.probe(False, 'DIR END', 400)
        with self.media() as patch:
            self.fat_entry(patch, 3, self.starts['KERNA.BIN'])
            self.probe(False, 'DIR OVERLAP')
        with self.media() as patch:
            self.fat_entry(patch, 2, 4, copies=(1,))
            self.probe(False, 'DIR FAT2')

    def test_file_chains_lengths_duplicates_and_overlap(self):
        for name, value in [('KERNA.BIN', 0), ('KERNA.BIN', 511), ('KERNA.BIN', 512),
                            ('KERNA.BIN', image.MAX_IMAGE + 512), ('BOOTCT0.BIN', 512)]:
            with self.subTest(name=name, bytes=value), self.media() as patch:
                patch(DATA * 512 + self.entries[name] + 28, struct.pack('<I', value))
                self.probe(False, 'FILE MAP')
        with self.media() as patch:
            self.dir_start(patch, 'KERNB.BIN', self.starts['KERNA.BIN'])
            self.probe(False, 'FILE OVERLAP')
        for nxt in (0, 1, 0xFFFFFF7, 0xFFFFFFF, self.starts['KERNA.BIN']):
            with self.subTest(next=nxt), self.media() as patch:
                self.fat_entry(patch, self.starts['KERNA.BIN'], nxt)
                self.probe(False, 'FILE MAP')
        with self.media() as patch:
            self.fat_entry(patch, self.starts['KERNA.BIN'], 0xFFFFFFF, copies=(1,))
            self.probe(False, 'FILE MAP')
        for attr in (0x10, 0x08):
            with self.subTest(attr=attr), self.media() as patch:
                patch(DATA * 512 + self.entries['KERNA.BIN'] + 11, bytes([attr]))
                self.probe(False, 'DIR DUP', 4)
        with self.media() as patch:
            patch(DATA * 512 + 128, self.root[32:64])
            self.probe(False, 'DIR DUP', 4)

    def test_exact_file_size_after_finite_mapping(self):
        for name in ('KERNA.BIN', 'BOOTCT0.BIN'):
            with self.subTest(name=name), self.media() as patch:
                patch(DATA * 512 + self.entries[name] + 28, struct.pack('<I', 512))
                self.fat_entry(patch, self.starts[name], 0xFFFFFFF)
                self.probe(False, 'FILE SIZE')
        with self.media() as patch:
            patch(DATA * 512 + self.entries['KERNB.BIN'], b'\xe5')
            self.probe(False, 'FILE MAP')

    def test_seeded_valid_fragmented_controls_and_last_cluster(self):
        rng = random.Random(SEED ^ 0x600D)
        for iteration in range(32):
            with self.subTest(seed=SEED ^ 0x600D, iteration=iteration), self.media() as patch:
                # Fragment valid controls independently of slot contents/geometry.
                chain = rng.sample(range(20000, 24000), 7) + [258017]
                for old in range(self.starts['BOOTCT1.BIN'], self.starts['BOOTCT1.BIN'] + 8):
                    self.fat_entry(patch, old, 0)
                self.dir_start(patch, 'BOOTCT1.BIN', chain[0])
                for at, cluster in enumerate(chain):
                    self.fat_entry(patch, cluster, chain[at + 1] if at + 1 < len(chain) else 0xFFFFFFF)
                patch((DATA + chain[0] - 2) * 512, self.record)
                self.probe()
                class Extent(C.Structure):
                    _fields_ = [('sectors', C.c_uint32 * 4097), ('count', C.c_uint32), ('bytes', C.c_uint32)]
                controls = (Extent * 2).in_dll(self.c, 'controls')
                self.assertEqual(controls[1].count, 8)
                self.assertEqual(list(controls[1].sectors[:8]), [DATA + c - 2 for c in chain])
                self.assertEqual(controls[1].sectors[7], 264191)

    def test_control_head_spacing_without_extent_overlap(self):
        for distance in range(1, 10):
            with self.subTest(distance=distance), self.media() as patch:
                for i in (0, 1):
                    head = 20000 + i * distance
                    tail = list(range(21000 + i * 16, 21007 + i * 16))
                    chain = [head] + tail
                    name = f'BOOTCT{i}.BIN'
                    for old in range(self.starts[name], self.starts[name] + 8):
                        self.fat_entry(patch, old, 0)
                    self.dir_start(patch, name, head)
                    for at, cluster in enumerate(chain):
                        self.fat_entry(patch, cluster, chain[at + 1] if at + 1 < len(chain) else 0xFFFFFFF)
                    patch((DATA + head - 2) * 512, self.record)
                self.probe(distance >= 8, 'none' if distance >= 8 else 'CTRL SPACING')


if __name__ == '__main__':
    unittest.main()
