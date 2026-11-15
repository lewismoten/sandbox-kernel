# SPDX-License-Identifier: MIT
"""Real COW FAT algorithm, cooperative worker and ABI over virtual slow SD.
No sleeping, deliberate process crashes, sanitizers, board or USB access.
"""
import ctypes as C
import os
import struct
import unittest
from test_m1_user import CallsBase, IMAGE, R, W, poke, peek, InstallBase
from test_cache import sparse_copy


class AsyncCache(CallsBase):
    read = InstallBase.read

    def stage(self, data, path='/sd/OS/CACHE/GAMES.TMP'):
        rc, fd = self.open(path, 0x601)
        self.assertEqual(rc, 0)
        base = IMAGE + 0x4000
        if self.c.us_check(base, len(data) or 1, W):
            self.assertEqual(self.c.us_map(base, (len(data)+4095)//4096*4096 or 4096, R | W), 0)
        poke(self.c, base, data)
        self.assertEqual(self.io(3, fd, base, len(data)), (0, len(data)))
        return fd

    def submit(self, fd, to='/sd/OS/CACHE/GAMES.IDX'):
        addr, n = 0, 0
        if to is not None:
            b = to.encode(); addr = self.BUF; n = len(b); poke(self.c, addr, b)
        return self.call(22, self.hdr(40)+struct.pack('<3Q', fd, addr, n))

    def status(self):
        before = self.c.read_count(), self.c.write_count(), self.c.micros()
        self.assertEqual(self.call(23, self.hdr(32)+bytes(16)), 0)
        self.assertEqual((self.c.read_count(), self.c.write_count(), self.c.micros()), before)
        return struct.unpack_from('<Qq', peek(self.c, self.REC, 32), 16)

    def finish(self, budget=1000):
        maximum = 0
        for _ in range(500000):
            if self.status()[0] != 1:
                return self.status(), maximum
            before = self.c.micros()
            self.c.cache_commit_task()
            maximum = max(maximum, (self.c.micros()-before) & 0xffffffff)
            self.assertLessEqual(maximum, budget)
        self.fail('worker did not finish')

    def test_large_slow_card_bound_completion_busy_and_legacy(self):
        data = bytes(range(256))*4096  # full 1 MiB
        fd = self.stage(data)
        self.c.inject_sd_latency(10000)  # 10ms every sector read AND write
        before = self.c.micros()
        self.assertEqual(self.call(12, self.hdr(24)+struct.pack('<Q', fd)), 0)
        sync_us = (self.c.micros()-before) & 0xffffffff
        self.assertGreater(sync_us, 2000000)
        self.assertEqual(self.status(), (0, 0))  # legacy fsync didn't queue
        self.assertEqual(self.close(fd), 0)
        self.c.inject_sd_latency(0)
        fd = self.stage(data)
        before = self.c.micros(), self.c.write_count(), self.c.read_count()
        self.assertEqual(self.submit(fd), 0)
        self.assertEqual((self.c.micros(), self.c.write_count(), self.c.read_count()), before)
        self.assertEqual(self.close(fd), -9)  # submission consumed handle
        self.assertEqual(self.status(), (1, 0))
        self.assertEqual(self.open('/sd/OS/SAVES/DICE/A.SAV', 0x601)[0], -16)
        self.assertEqual(self.open('/sd/S512.BIN')[0], -16)
        self.assertEqual(self.c.fatput(b'OTHER.BIN', b'x', 1), -16)
        self.assertEqual(self.c.fatcache_remove(b'GAMES.TMP'), -16)
        self.c.inject_sd_latency(10000)
        # Poll, frame-independent health and input/device calls stay available.
        self.assertEqual(self.call(17, self.hdr(32)+struct.pack('<2Q', 1, 1)), 0)
        self.assertEqual(self.open('/dev/log', 1)[0], 0)
        state, max_us = self.finish()
        self.assertEqual(state, (2, 0))
        self.assertEqual(self.status(), (2, 0))
        self.c.inject_sd_latency(0)
        self.assertEqual(self.read('/OS/CACHE/GAMES.IDX'), data)
        print(f'async 1MiB slow SD (10ms/read or write): legacy fsync={sync_us} us; submit/poll=0 us; max worker={max_us} us')

    def test_existing_asset_handles_busy_then_resume_without_losing_position(self):
        rc, reader = self.open('/sd/S512.BIN'); self.assertEqual(rc, 0)
        rc, directory = self.open('/sd'); self.assertEqual(rc, 0)
        self.assertEqual(self.io(2, reader, self.BUF, 4), (0, 4))
        fd = self.stage(b'new')
        self.assertEqual(self.submit(fd), 0)
        self.assertEqual(self.io(2, reader, self.BUF, 4)[0], -16)
        self.assertEqual(self.io(20, directory, self.BUF, 512)[0], -16)
        self.assertEqual(self.call(9, self.hdr(48)+struct.pack('<Qq2Q', reader, 0, 0, 0)), -16)
        self.assertEqual(self.finish()[0], (2, 0))
        self.assertEqual(self.io(2, reader, self.BUF, 4), (0, 4))
        self.assertEqual(peek(self.c, self.BUF, 4), bytes(range(4, 8)))
        self.assertEqual(self.io(20, directory, self.BUF, 512)[0], 0)
        self.assertEqual(self.close(reader), 0); self.assertEqual(self.close(directory), 0)

    def test_error_reported_retained_retry_and_old_file(self):
        self.c.fatcache_put.argtypes = [C.c_char_p, C.c_char_p, C.c_uint32]
        self.assertEqual(self.c.fatcache_put(b'GAMES.IDX', b'old', 3), 0)
        fd = self.stage(b'new'*4096)
        self.assertEqual(self.submit(fd), 0)
        self.c.inject_failure(self.c.write_count()+2)
        state, _ = self.finish()
        self.assertEqual(state, (3, -5))
        self.assertEqual(self.status(), state)
        self.assertEqual(self.read('/OS/CACHE/GAMES.IDX'), b'old')
        self.c.inject_failure(0xffffffff)
        fd = self.stage(b'retry')
        self.assertEqual(self.submit(fd), 0)
        self.assertEqual(self.finish()[0], (2, 0))
        self.assertEqual(self.read('/OS/CACHE/GAMES.IDX'), b'retry')

    def test_saves_scope_zero_length_reset_and_writer_ownership(self):
        fd = self.stage(b'', '/sd/OS/SAVES/DICE/EMPTY.SAV')
        self.assertEqual(self.submit(fd, '/sd/OS/SAVES/OTHER/X.SAV'), -22)
        self.assertEqual(self.submit(fd, '/sd/OS/CACHE/X.IDX'), -22)
        self.assertEqual(self.submit(fd, None), 0)
        self.c.sys_reset()  # accepted bytes outlive the app; no user pointers
        self.assertEqual(self.open('/sd/OS/CACHE/X.TMP', 0x601)[0], -16)
        self.assertEqual(self.finish()[0], (2, 0))
        self.assertEqual(self.read('/OS/SAVES/DICE/EMPTY.SAV'), b'')
        fd = self.stage(b'save bytes'*200, '/sd/OS/SAVES/DICE/FINAL.SAV')
        self.assertEqual(self.submit(fd, None), 0)
        self.assertEqual(self.finish()[0], (2, 0))
        self.assertEqual(self.read('/OS/SAVES/DICE/FINAL.SAV'), b'save bytes'*200)
        self.assertEqual(self.open('/sd/OS/CACHE/X.TMP', 0x601)[0], 0)

    def test_records_and_pointers_fail_without_consuming_writer(self):
        fd = self.stage(b'bytes')
        writes = self.c.write_count()
        for call, size in ((22, 40), (23, 32)):
            for off in (4, 8, 12):
                rec = bytearray(self.hdr(size)+bytes(size-16)); struct.pack_into('<I', rec, off, 1)
                self.assertEqual(self.call(call, bytes(rec)), -22)
            self.assertEqual(self.call(call, self.hdr(size)+bytes(size-16), major=2), -93)
            self.assertEqual(self.call(call, self.hdr(size+8)+bytes(size-16)+b'\1'+bytes(7)), -22)
        self.assertEqual(self.call(23, self.hdr(32)+struct.pack('<2Q', 1, 0)), -22)
        for addr, size, error in ((1<<32, 10, -14), (0, 1, -22), (self.BUF, 256, -36), (0x1000, 10, -14)):
            self.assertEqual(self.call(22, self.hdr(40)+struct.pack('<3Q', fd, addr, size)), error)
        self.assertEqual(self.c.write_count(), writes)
        self.assertEqual(self.close(fd), 0)

    def test_all_sector_write_interruptions_remount_old_or_new(self):
        # Fault injection stops durable whole-sector writes, not a process crash.
        self.c.fatcache_put.argtypes = [C.c_char_p, C.c_char_p, C.c_uint32]
        old, new = b'old'*300, b'new'*700
        self.assertEqual(self.c.fatcache_put(b'GAMES.IDX', old, len(old)), 0)
        baseline = self.work/'async-baseline.img'; sparse_copy(self.disk, baseline)
        fd = self.stage(new); start = self.c.write_count()
        self.assertEqual(self.submit(fd), 0); self.assertEqual(self.finish()[0], (2, 0))
        writes = self.c.write_count()-start
        for cut in range(writes+1):
            with self.subTest(cut=cut):
                self.c.close_disk(); sparse_copy(baseline, self.disk)
                self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                fd = self.stage(new); self.c.inject_failure(self.c.write_count()+cut)
                self.assertEqual(self.submit(fd), 0)
                state, _ = self.finish()
                self.assertEqual(state[0] == 2, cut == writes)
                self.c.inject_failure(0xffffffff)
                self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk))
                self.assertEqual(self.c.asset_mount(), 0)
                self.assertIn(self.read('/OS/CACHE/GAMES.IDX'), (old, new))
                self.assertEqual(self.read('/S512.BIN'), bytes(range(256))*2)
        print('async COW sector-write interruption points:', writes+1)
