# SPDX-License-Identifier: MIT
"""Real protocol/store/FAT COW worker over fake CDC and whole-sector SD.
Explicit pass counts select each existing update phase, without a production
phase hook or stubbed pending bit. This is host evidence, not a board test.
"""
import ctypes as C
import os
import unittest
import test_m0 as m0
import test_m1_user as user


class UpdateCacheBounds(unittest.TestCase):
    setUpClass = m0.CoreTests.__dict__['setUpClass']
    tearDownClass = m0.CoreTests.__dict__['tearDownClass']
    setUp = m0.CoreTests.setUp
    tearDown = m0.CoreTests.tearDown
    confirmed_hash = m0.CoreTests.confirmed_hash
    cmd = m0.CoreTests.cmd
    begin = m0.CoreTests.begin
    transfer = m0.CoreTests.transfer
    install = m0.CoreTests.install
    read = user.InstallBase.read

    def enter(self, phase):
        self.assertEqual(self.c.asset_mount(), 0)
        self.c.cache_commit_pending.restype = C.c_bool
        self.c.cache_commit_start.argtypes = [C.c_char_p, C.c_char_p, C.c_void_p, C.c_uint32]
        self.c.asset_open.argtypes = [C.c_char_p, C.c_uint32, C.c_uint32]
        self.c.asset_read.argtypes = [C.c_int, C.c_void_p, C.c_uint32]
        self.c.fatcache_put.argtypes = [C.c_char_p, C.c_char_p, C.c_uint32]
        self.assertEqual(self.c.fatcache_put(b'GAMES.IDX', b'old index', 9), 0)
        self.c.take_output()
        self.payload = m0.image.pack(b'B'*768, 'cache-overlap')  # two sectors
        self.assertEqual(len(self.payload), 1024)
        self.assertFalse(self.c.cache_commit_pending())
        self.assertEqual(self.begin(self.payload), 'READY 1024\n')
        self.transfer(self.payload)
        base_writes = self.c.write_count()
        # phase 2 -> 3 -> 4; two writes -> 5; two reads -> 6.
        for _ in range(2 + (2 if phase >= 5 else 0) + (2 if phase >= 6 else 0)):
            self.c.protocol_task()
            self.assertEqual(self.c.take_output(), b'')
        self.assertEqual(self.c.write_count()-base_writes, 0 if phase == 4 else 2)
        self.assertTrue(self.c.protocol_busy())
        self.assertTrue(C.c_bool.in_dll(self.c, 'updates_ready').value)

    def start_real_cow(self):
        self.data = bytes(range(256))*256
        self.pending_data = C.create_string_buffer(self.data)
        self.assertEqual(self.c.cache_commit_start(bytes(9), b'GAMES.IDX\0\0\0\0',
                                                  self.pending_data, len(self.data)), 0)
        self.assertTrue(self.c.cache_commit_pending())

    def overlap(self, phase):
        self.enter(phase)
        self.start_real_cow()  # strictly AFTER PUT2 admission and phase selection
        before = self.c.read_count(), self.c.write_count()
        for _ in range(3):
            self.c.protocol_task()
            self.assertEqual(self.c.take_output(), b'')
            self.assertTrue(self.c.protocol_busy())
            self.assertTrue(C.c_bool.in_dll(self.c, 'updates_ready').value)
        self.assertEqual((self.c.read_count(), self.c.write_count()), before)
        passes = 0
        while self.c.cache_commit_pending():
            self.c.protocol_task()
            self.assertEqual(self.c.take_output(), b'')
            self.assertTrue(C.c_bool.in_dll(self.c, 'updates_ready').value)
            self.c.cache_commit_task()
            passes += 1
            self.assertLess(passes, 20000)
        self.assertEqual((self.c.cache_commit_state(), self.c.cache_commit_error()), (2, 0))
        self.assertGreater(passes, 128)  # actual FAT/data operations, not a fake flag
        self.assertEqual(self.install(), 'OK pending rebooting\n')
        self.assertTrue(C.c_bool.in_dll(self.c, 'updates_ready').value)
        self.assertEqual(self.read('/OS/CACHE/GAMES.IDX'), self.data)
        ctl = (C.c_uint32*128).in_dll(self.c, 'control')
        self.assertEqual((ctl[3], ctl[4]), (0, 1))
        print(f'phase {phase}: real COW pending -> wait -> complete ({passes} worker passes), update remains ready')

    def test_phase4_waits_real_cow(self): self.overlap(4)
    def test_phase5_waits_real_cow(self): self.overlap(5)
    def test_phase6_waits_real_cow(self): self.overlap(6)

    def test_pending_wait_still_consumes_install_deadline(self):
        self.enter(4)
        self.start_real_cow()
        before = self.c.read_count(), self.c.write_count()
        self.c.advance(120000001)
        self.c.protocol_task()
        self.assertEqual(self.c.take_output(), b'ERR install-timeout\n')
        self.assertFalse(self.c.protocol_busy())
        self.assertTrue(C.c_bool.in_dll(self.c, 'updates_ready').value)
        self.assertEqual((self.c.read_count(), self.c.write_count()), before)
        while self.c.cache_commit_pending(): self.c.cache_commit_task()
        self.assertEqual(self.c.cache_commit_state(), 2)

    def failure(self, phase, reason):
        self.enter(phase)
        if phase == 5:
            self.c.fail_read_sector(self.layout['KERNB.BIN']['first_lba'])
        else:
            self.c.inject_failure(self.c.write_count())
        self.c.protocol_task()
        self.assertEqual(self.c.take_output().decode(), 'ERR '+reason+'\n')
        self.assertFalse(C.c_bool.in_dll(self.c, 'updates_ready').value)
        ctl = (C.c_uint32*128).in_dll(self.c, 'control')
        self.assertEqual(ctl[4], m0.image.NO_SLOT)

    def test_phase4_true_write_failure_still_locks(self): self.failure(4, 'write')
    def test_phase5_true_read_failure_still_locks(self): self.failure(5, 'readback')
    def test_phase6_true_commit_failure_still_locks(self): self.failure(6, 'commit-uncertain-query-info')

    def test_trailing_input_not_hidden_by_pending(self):
        self.enter(4)
        self.start_real_cow()
        self.assertEqual(self.cmd(b'X'), 'ERR trailing-input\n')
        self.assertTrue(C.c_bool.in_dll(self.c, 'updates_ready').value)
        while self.c.cache_commit_pending(): self.c.cache_commit_task()


if __name__ == '__main__': unittest.main()
