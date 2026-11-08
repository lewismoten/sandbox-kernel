# SPDX-License-Identifier: MIT
"""Bounded absolute seeks: checked syscall records and real FAT cursors.

Fixtures translate user addresses before host memory access. No ARM execution.
"""
import ctypes as C
import os
import pathlib
import random
import shutil
import struct
import tempfile
import unittest

import fatgen
import hostlib
import test_m1_user as m1


class Seek(m1.CallsBase):
    def seek(self, fd, offset, whence=0, position=0):
        raw = self.hdr(48) + struct.pack('<QqQQ', fd, offset, whence, position)
        status = self.call(9, raw)
        return status, struct.unpack_from('<Q', m1.peek(self.c, self.REC, 48), 40)[0]

    def test_forward_backward_read_after_seek(self):
        status, fd = self.open('/sd/S512.BIN')
        self.assertEqual(status, 0)
        for offset in (300, 5, 511, 0):
            self.assertEqual(self.seek(fd, offset), (0, offset))
            self.assertEqual(self.io(2, fd, self.BUF, 1), (0, 1))
            self.assertEqual(m1.peek(self.c, self.BUF, 1), bytes([offset % 256]))

    def test_eof_and_failed_seek_preserve_position(self):
        _, fd = self.open('/sd/S512.BIN')
        self.assertEqual(self.seek(fd, 512), (0, 512))
        self.assertEqual(self.io(2, fd, self.BUF, 17), (0, 0))
        self.assertEqual(self.seek(fd, 27), (0, 27))
        for offset, whence in ((-1, 0), (513, 0), (1 << 32, 0),
                               ((1 << 63) - 1, 0), (0, 1), (0, 2), (0, (1 << 64) - 1)):
            with self.subTest(offset=offset, whence=whence):
                self.assertEqual(self.seek(fd, offset, whence), (-22, 0))
                self.assertEqual(self.io(2, fd, self.BUF, 1), (0, 1))
                self.assertEqual(m1.peek(self.c, self.BUF, 1), b'\x1b')
                self.assertEqual(self.seek(fd, 27), (0, 27))

    def test_invalid_closed_stale_and_nonfile_handles(self):
        self.c.input_init()
        for bad in (0, 1, (1 << 64) - 1, 1 << 24, 1 << 32):
            self.assertEqual(self.seek(bad, 0), (-9, 0))
        _, fd = self.open('/sd/S512.BIN')
        self.assertEqual(self.close(fd), 0)
        _, replacement = self.open('/sd/S512.BIN')
        self.assertNotEqual(fd, replacement)
        self.assertEqual(self.seek(fd, 0), (-9, 0))
        for path, flags in (('/sd', 0), ('/dev/log', 1), ('/dev/fb0', 2),
                            ('/dev/input/event0', 0x100)):
            status, handle = self.open(path, flags)
            self.assertEqual(status, 0)
            self.assertEqual(self.seek(handle, 0), (-29, 0))
            self.assertEqual(self.close(handle), 0)

    def test_record_failures_are_atomic_and_zero_tail_supported(self):
        _, fd = self.open('/sd/S512.BIN')
        self.assertEqual(self.seek(fd, 31), (0, 31))
        good = self.hdr(48) + struct.pack('<QqQQ', fd, 400, 0, 0)
        bad_records = [self.hdr(40) + good[16:40],
                       self.hdr(56) + good[16:] + b'\x01' + bytes(7)]
        for at in (4, 8, 12, 40):
            bad = bytearray(good)
            bad[at] = 1
            bad_records.append(bytes(bad))
        for raw in bad_records:
            self.assertEqual(self.call(9, raw), -22)
            self.assertEqual(m1.peek(self.c, self.REC, len(raw)), raw)
            self.assertEqual(self.io(2, fd, self.BUF, 1), (0, 1))
            self.assertEqual(m1.peek(self.c, self.BUF, 1), b'\x1f')
            self.assertEqual(self.seek(fd, 31), (0, 31))
        # A readable but read-only record must not move the cursor.
        ro = m1.IMAGE + 0x100
        m1.poke(self.c, ro, good)
        self.assertEqual(self.call(9, None, size=48, va=ro), -14)
        self.assertEqual(self.io(2, fd, self.BUF, 1), (0, 1))
        self.assertEqual(m1.peek(self.c, self.BUF, 1), b'\x1f')
        extended = self.hdr(56) + good[16:] + bytes(8)
        self.assertEqual(self.call(9, extended), 0)
        self.assertEqual(struct.unpack_from('<Q', m1.peek(self.c, self.REC, 56), 40)[0], 400)
        self.assertEqual(m1.peek(self.c, self.REC + 48, 8), bytes(8))

    def test_legacy_sequential_read_open_close_unchanged(self):
        # Exact ABI 1.0-1.2 wire records, without any new seek use.
        _, fd = self.open('/sd/S512.BIN')
        result = bytearray()
        for count in (13, 200, 299, 1):
            status, got = self.io(2, fd, self.BUF, count)
            self.assertEqual(status, 0)
            result += m1.peek(self.c, self.BUF, got)
        self.assertEqual(bytes(result), bytes(range(256)) * 2)
        self.assertEqual(self.close(fd), 0)
        self.assertEqual(self.io(2, fd, self.BUF, 1)[0], -9)


class AssetSeek(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        cls.library = hostlib.build(cls.work / 'core.so')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def load(self, size=70001, spc=1, fragment=False):
        self.case = pathlib.Path(tempfile.mkdtemp(dir=self.work))
        shutil.copyfile(self.library, self.case / 'core.so')
        self.c = C.CDLL(str(self.case / 'core.so'))
        self.c.open_disk.argtypes = [C.c_char_p]
        self.c.asset_open.argtypes = [C.c_char_p, C.c_uint32, C.c_uint32]
        self.c.asset_seek.argtypes = [C.c_int, C.c_uint64]
        self.c.asset_read.argtypes = [C.c_int, C.c_void_p, C.c_uint32]
        self.payload = random.Random(1107).randbytes(size)
        self.disk = self.case / 'card.img'
        self.vol = fatgen.build(self.disk, {'PACK.GMP': self.payload, 'EMPTY': b'', 'DIR': {}}, spc, fragment)
        self.c.open_disk(os.fsencode(self.disk))
        self.addCleanup(self.finish)
        self.assertEqual(self.c.asset_mount(), 0)
        self.fd = self.c.asset_open(b'/PACK.GMP', 9, 0)
        self.assertGreaterEqual(self.fd, 0)
        self.buffer = C.create_string_buffer(4096)

    def finish(self):
        self.assertEqual(self.c.write_count(), 0)
        self.c.close_disk()

    def read(self, fd, count):
        got = self.c.asset_read(fd, self.buffer, count)
        return got, self.buffer.raw[:max(0, got)]

    def test_sector_cluster_boundaries_random_access_and_independent_handles(self):
        for spc, fragmented in ((1, False), (2, False), (1, True), (2, True)):
            with self.subTest(spc=spc, fragmented=fragmented):
                self.load(spc=spc, fragment=fragmented)
                other = self.c.asset_open(b'/PACK.GMP', 9, 0)
                offsets = [0, 1, 511, 512, 513, 1023, 1024, 1025, 4096,
                           len(self.payload), len(self.payload) - 1, 0]
                offsets += [random.Random(i).randrange(len(self.payload) + 1) for i in range(120)]
                for offset in offsets:
                    self.assertEqual(self.c.asset_seek(self.fd, offset), 0)
                    want = self.payload[offset:offset + 1301]
                    self.assertEqual(self.read(self.fd, 1301), (len(want), want))
                self.assertEqual(self.read(other, 31), (31, self.payload[:31]))
                self.doCleanups()

    def test_empty_aligned_and_one_byte_files(self):
        for size in (0, 1, 512, 1024, 1025):
            self.load(size=size)
            for offset in (size, 0, size):
                self.assertEqual(self.c.asset_seek(self.fd, offset), 0)
                want = self.payload[offset:offset + 17]
                self.assertEqual(self.read(self.fd, 17), (len(want), want))
            self.assertEqual(self.c.asset_seek(self.fd, size + 1), -22)
            self.doCleanups()

    def test_large_file_seek_and_repeated_forward_skips_without_replay(self):
        self.load(size=2400000, fragment=True)
        for offset in (2300000, 200, 1200000, 1201000, 2400000, 0):
            before = self.c.read_count()
            self.assertEqual(self.c.asset_seek(self.fd, offset), 0)
            # FAT links are traversed; skipped payload sectors are not read.
            self.assertLess(self.c.read_count() - before, 80)
            want = self.payload[offset:offset + 43]
            self.assertEqual(self.read(self.fd, 43), (len(want), want))

    def test_direct_api_errors_preserve_position_and_handle_generations(self):
        self.load()
        self.assertEqual(self.c.asset_seek(self.fd, 37), 0)
        for offset in (len(self.payload) + 1, 1 << 32, (1 << 64) - 1):
            self.assertEqual(self.c.asset_seek(self.fd, offset), -22)
        self.assertEqual(self.read(self.fd, 1), (1, self.payload[37:38]))
        directory = self.c.asset_open(b'/DIR', 4, 0)
        self.assertEqual(self.c.asset_seek(directory, 0), -29)
        for handle in (-1, 0, 1 << 30):
            self.assertEqual(self.c.asset_seek(handle, 0), -9)
        old = self.fd
        self.assertEqual(self.c.asset_close(old), 0)
        self.fd = self.c.asset_open(b'/PACK.GMP', 9, 0)
        self.assertNotEqual(self.fd, old)
        self.assertEqual(self.c.asset_seek(old, 0), -9)
        self.assertEqual(self.c.asset_mount(), 0)
        self.assertEqual(self.c.asset_seek(self.fd, 0), -9)

    def test_corrupt_chain_failed_seek_is_atomic_and_bounded(self):
        self.load(size=4096, fragment=True)
        self.assertEqual(self.c.asset_seek(self.fd, 27), 0)
        first = self.vol.chains['/PACK.GMP'][0]
        for cluster in (first, self.vol.chains['/PACK.GMP'][2]):
            original = self.vol.fat[cluster]
            for link in (0, 0x0ffffff7, fatgen.EOC, self.vol.clusters + 2):
                fatgen.set_fat(self.disk, self.vol, cluster, link)
                # Discard FILE* read-ahead; preserve the live handle/cursor.
                self.c.close_disk()
                self.c.open_disk(os.fsencode(self.disk))
                self.assertEqual(self.c.asset_seek(self.fd, 3000), -5)
                self.assertEqual(self.read(self.fd, 1), (1, self.payload[27:28]))
                self.assertEqual(self.c.asset_seek(self.fd, 27), 0)
            fatgen.set_fat(self.disk, self.vol, cluster, original)
            self.c.close_disk()
            self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.c.asset_seek(self.fd, 3000), 0)
        self.assertEqual(self.read(self.fd, 31), (31, self.payload[3000:3031]))

    def test_fat_io_failure_does_not_move_seek_cursor(self):
        self.load(size=4096)
        self.assertEqual(self.c.asset_seek(self.fd, 27), 0)
        self.c.close_disk()
        with self.disk.open('r+b') as disk:
            disk.truncate(self.vol.fat_lba * 512)
        self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.c.asset_seek(self.fd, 3000), -5)
        self.c.close_disk()
        # Restore the exact deterministic medium, not the live asset handle.
        fatgen.build(self.disk, {'PACK.GMP': self.payload, 'EMPTY': b'', 'DIR': {}})
        self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.read(self.fd, 31), (31, self.payload[27:58]))

    def test_failed_seek_preserves_pending_error_success_clears_it(self):
        self.load(size=4096)
        first = self.vol.chains['/PACK.GMP'][0]
        fatgen.set_fat(self.disk, self.vol, first, fatgen.EOC)
        self.assertEqual(self.read(self.fd, 1024), (512, self.payload[:512]))
        self.assertEqual(self.c.asset_seek(self.fd, 4097), -22)
        self.assertEqual(self.read(self.fd, 1), (-5, b''))
        # Recreate a deferred error; successful rewind deliberately clears it.
        self.assertEqual(self.c.asset_seek(self.fd, 0), 0)
        self.assertEqual(self.read(self.fd, 1024), (512, self.payload[:512]))
        self.assertEqual(self.c.asset_seek(self.fd, 0), 0)
        self.assertEqual(self.read(self.fd, 31), (31, self.payload[:31]))

    def test_seek_after_data_io_failure_at_cluster_boundary(self):
        self.load(size=4096)
        second = self.vol.chains['/PACK.GMP'][1]
        cut = self.vol.lba(second) * 512
        length = self.disk.stat().st_size
        self.c.close_disk()
        with self.disk.open('r+b') as disk:
            disk.truncate(cut)
        self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.read(self.fd, 1024), (512, self.payload[:512]))
        # Restore media, not handles, to check recovery of the same cursor.
        self.c.close_disk()
        with self.disk.open('r+b') as disk:
            disk.truncate(length)
            for lba, data in self.vol.sectors.items():
                if lba * 512 >= cut:
                    disk.seek(lba * 512)
                    disk.write(data)
        self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.c.asset_seek(self.fd, 0), 0)
        self.assertEqual(self.read(self.fd, 31), (31, self.payload[:31]))
