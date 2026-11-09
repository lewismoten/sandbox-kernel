# SPDX-License-Identifier: MIT
"""Scoped save writes: /sd/OS/SAVES/<GAME>/<NAME> only, copy-on-write, power-cut safe."""
import ctypes as C
import os
import struct
import unittest
from test_m1_user import InstallBase, CallsBase, poke, peek, fsck
from test_cache import sparse_copy

class AbiMinor(unittest.TestCase):
    def test_both_headers_say_1_7(self):
        import re, pathlib
        root = pathlib.Path(__file__).resolve().parents[1]
        for path, name in (('include/abi.h', 'K_ABI'), ('sdk/include/sandbox/abi.h', 'SBX_ABI')):
            text = (root / path).read_text()
            self.assertEqual(re.search(name + r'_MAJOR (\d+)u', text).group(1), '1')
            self.assertEqual(re.search(name + r'_MINOR (\d+)u', text).group(1), '8', path)
        self.assertIn('1.7', (root / 'docs/saves-storage.md').read_text())

class SaveStorage(InstallBase):
    def put(self, game, name, data):
        self.c.fatsave_put.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint32]
        return self.c.fatsave_put(game, name, data, len(data))

    def test_create_replace_and_read_back_in_game_folder(self):
        self.assertEqual(self.put(b'DICE', b'DICE.HIS', b'a' * 340), 0)
        self.assertEqual(self.read('/OS/SAVES/DICE/DICE.HIS'), b'a' * 340)
        self.assertEqual(self.put(b'DICE', b'DICE.HIS', b'b' * 5000), 0)
        self.assertEqual(self.read('/OS/SAVES/DICE/DICE.HIS'), b'b' * 5000)
        self.assertEqual(self.put(b'OTHER', b'GAME0.SAV', b'x'), 0)
        self.assertEqual(self.read('/OS/SAVES/DICE/DICE.HIS'), b'b' * 5000)
        self.untouched()
        r = fsck(self.disk, self.work)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_bad_game_or_file_names_are_refused_before_any_write(self):
        before = self.c.write_count()
        for game in (b'', b'dice', b'A.B', b'TOOLONGNAME', b'../X', b'A B'):
            with self.subTest(game=game): self.assertEqual(self.put(game, b'X.HIS', b'x'), -22)
        self.assertEqual(self.put(b'DICE', b'x.his', b'x'), -22)
        self.assertEqual(self.c.write_count(), before)

    def test_every_rename_write_cut_leaves_old_or_new_never_a_torn_file(self):
        old, new = b'old' * 500, b'new' * 500
        self.assertEqual(self.put(b'DICE', b'GAME0.SAV', old), 0)
        self.assertEqual(self.put(b'DICE', b'GAME0.TMP', new), 0)
        baseline = self.work / 'saves-baseline.img'
        sparse_copy(self.disk, baseline)
        self.c.fatsave_rename.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_void_p, C.c_uint32]
        buf = C.create_string_buffer(1 << 20)
        start = self.c.write_count()
        self.assertEqual(self.c.fatsave_rename(b'DICE', b'GAME0.TMP', b'GAME0.SAV', buf, len(buf)), 0)
        steps = self.c.write_count() - start
        self.assertGreater(steps, 5)
        for cut in range(steps + 1):
            with self.subTest(cut=cut):
                self.c.close_disk(); sparse_copy(baseline, self.disk)
                self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.c.inject_failure(self.c.write_count() + cut)
                result = self.c.fatsave_rename(b'DICE', b'GAME0.TMP', b'GAME0.SAV', buf, len(buf))
                self.assertEqual(result == 0, cut == steps)
                self.c.inject_failure(0xffffffff)
                self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.assertIn(self.read('/OS/SAVES/DICE/GAME0.SAV'), (old, new))
                for name, data in self.before.items(): self.assertEqual(self.read('/' + name), data)

    def test_every_first_write_cut_is_complete_or_missing(self):
        data = b'score' * 100
        baseline = self.work / 'first-baseline.img'
        sparse_copy(self.disk, baseline)
        start = self.c.write_count()
        self.assertEqual(self.put(b'DICE', b'DICE.HIS', data), 0)
        steps = self.c.write_count() - start
        for cut in range(steps + 1):
            with self.subTest(cut=cut):
                self.c.close_disk(); sparse_copy(baseline, self.disk)
                self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.c.inject_failure(self.c.write_count() + cut)
                self.put(b'DICE', b'DICE.HIS', data)
                self.c.inject_failure(0xffffffff)
                self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                h = self.c.asset_open(b'/OS/SAVES/DICE/DICE.HIS', 23, 0)
                if h >= 0: self.c.asset_close(h); self.assertEqual(self.read('/OS/SAVES/DICE/DICE.HIS'), data)
                else: self.assertEqual(h, -2)

    def test_a_read_only_save_is_not_replaced(self):
        import mmap
        self.assertEqual(self.put(b'DICE', b'DICE.HIS', b'keep'), 0)
        with open(self.disk, 'r+b') as f, mmap.mmap(f.fileno(), 0) as m:
            at = m.find(b'DICE    HIS'); self.assertGreater(at, 0)
            m[at + 11] = 0x01
        self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
        self.assertNotEqual(self.put(b'DICE', b'DICE.HIS', b'lose'), 0)
        self.assertEqual(self.read('/OS/SAVES/DICE/DICE.HIS'), b'keep')

    def test_folder_and_file_caps_return_a_clear_error_and_change_nothing(self):
        for i in range(64): self.assertEqual(self.put(b'G%d' % i, b'A.HIS', b'x'), 0, i)
        before = self.c.write_count()
        self.assertEqual(self.put(b'ONEMORE', b'A.HIS', b'x'), -122)
        self.assertEqual(self.c.write_count(), before)
        self.assertEqual(self.put(b'G3', b'A.HIS', b'new'), 0)  # an existing game still saves
        for i in range(31): self.assertEqual(self.put(b'G0', b'F%d.SAV' % i, b'x'), 0, i)
        before = self.c.write_count()
        self.assertEqual(self.put(b'G0', b'LAST.SAV', b'x'), -122)
        self.assertEqual(self.c.write_count(), before)
        self.assertEqual(self.put(b'G0', b'F5.SAV', b'again'), 0)  # replacing one is not a new file
        self.assertEqual(self.read('/OS/SAVES/G0/F5.SAV'), b'again')

    def corrupt(self, name):
        import mmap
        with open(self.disk, 'r+b') as f, mmap.mmap(f.fileno(), 0) as m:
            at = m.find(name); self.assertGreater(at, 0)
            m[at+28:at+32] = struct.pack('<I', (1 << 20) + 1)  # size no chain can hold
        self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)

    def test_a_save_with_a_corrupt_chain_can_be_replaced_or_removed(self):
        self.assertEqual(self.put(b'DICE', b'DICE.HIS', b'old'), 0)
        self.corrupt(b'DICE    HIS')
        self.assertEqual(self.put(b'DICE', b'DICE.HIS', b'fresh'), 0)  # the bad chain is left for fsck, never freed
        self.assertEqual(self.read('/OS/SAVES/DICE/DICE.HIS'), b'fresh')
        self.assertEqual(self.put(b'DICE', b'GAME0.SAV', b'old'), 0)
        self.corrupt(b'GAME0   SAV')
        self.c.fatsave_remove.argtypes = [C.c_char_p, C.c_char_p]
        self.assertEqual(self.c.fatsave_remove(b'DICE', b'GAME0.SAV'), 0)
        self.assertEqual(self.c.asset_open(b'/OS/SAVES/DICE/GAME0.SAV', 24, 0), -2)
        self.assertEqual(self.read('/OS/SAVES/DICE/DICE.HIS'), b'fresh')

class SaveCalls(CallsBase):
    def test_app_can_write_only_inside_one_game_folder(self):
        for name in ('/sd/OS/SAVES/DICE', '/sd/OS/SAVES/DICE/', '/sd/OS/SAVES/DICE/A/B', '/sd/OS/SAVES//X.HIS',
                     '/sd/OS/SAVES/dice/X.HIS', '/sd/OS/SAVES/TOOLONGNAME/X.HIS', '/sd/OS/SAVES/A.B/X.HIS',
                     '/sd/OS/SAVES/DICE/../X', '/sd/OS/SAVES/DICE/X.TOOLONG', '/sd/OS/SAVE/DICE/X.HIS',
                     '/sd/GAMES/DICE/X.HIS', '/sd/OS/SAVES/X.HIS'):
            with self.subTest(name=name): self.assertLess(self.open(name, 0x601)[0], 0)
        a = b'/sd/OS/SAVES/DICE/X\x00JUNK'
        poke(self.c, self.BUF, a)
        self.assertLess(self.open(a.decode('latin1'), 0x601)[0], 0)

    def test_write_sync_rename_commits_and_close_alone_aborts(self):
        r, fd = self.open('/sd/OS/SAVES/DICE/DICE.TMP', 0x601); self.assertEqual(r, 0)
        poke(self.c, self.BUF, b'high scores')
        self.assertEqual(self.io(3, fd, self.BUF, 11), (0, 11))
        self.assertEqual(self.close(fd), 0)  # no fsync: nothing is published
        self.assertLess(self.open('/sd/OS/SAVES/DICE/DICE.TMP')[0], 0)
        r, fd = self.open('/sd/OS/SAVES/DICE/DICE.TMP', 0x601); self.assertEqual(r, 0)
        self.assertEqual(self.io(3, fd, self.BUF, 11), (0, 11))
        self.assertEqual(self.call(12, self.hdr(24) + struct.pack('<Q', fd)), 0)
        self.assertEqual(self.close(fd), 0)
        a, b = b'/sd/OS/SAVES/DICE/DICE.TMP', b'/sd/OS/SAVES/DICE/DICE.HIS'
        poke(self.c, self.BUF, a); poke(self.c, self.BUF + 128, b)
        self.assertEqual(self.call(19, self.hdr(48) + struct.pack('<4Q', self.BUF, len(a), self.BUF + 128, len(b))), 0)
        r, fd = self.open(b.decode()); self.assertEqual(r, 0)
        self.assertEqual(self.io(2, fd, self.BUF, 100), (0, 11))
        self.assertEqual(peek(self.c, self.BUF, 11), b'high scores')
        self.assertLess(self.open(a.decode())[0], 0)

    def test_rename_never_crosses_folders(self):
        for a, b in ((b'/sd/OS/SAVES/DICE/A.TMP', b'/sd/OS/SAVES/OTHER/A.HIS'),
                     (b'/sd/OS/SAVES/DICE/A.TMP', b'/sd/OS/CACHE/A.HIS'),
                     (b'/sd/OS/CACHE/A.TMP', b'/sd/OS/SAVES/DICE/A.HIS')):
            with self.subTest(a=a, b=b):
                poke(self.c, self.BUF, a); poke(self.c, self.BUF + 128, b)
                before = self.c.write_count()
                self.assertEqual(self.call(19, self.hdr(48) + struct.pack('<4Q', self.BUF, len(a), self.BUF + 128, len(b))), -22)
                self.assertEqual(self.c.write_count(), before)

    def test_one_writer_and_open_reader_block_mutation(self):
        r, fd = self.open('/sd/OS/SAVES/DICE/A.HIS', 0x601); self.assertEqual(r, 0)
        self.assertEqual(self.open('/sd/OS/SAVES/OTHER/B.HIS', 0x601)[0], -16)
        self.assertEqual(self.open('/sd/OS/CACHE/B.IDX', 0x601)[0], -16)

    def test_cache_reader_blocks_a_save_and_a_save_reader_blocks_the_cache(self):
        self.c.fatcache_put.argtypes = [C.c_char_p, C.c_char_p, C.c_uint32]
        self.c.fatsave_put.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint32]
        self.assertEqual(self.c.fatcache_put(b'GAMES.IDX', b'c', 1), 0)
        self.assertEqual(self.c.fatsave_put(b'DICE', b'DICE.HIS', b's', 1), 0)
        r, rd = self.open('/sd/OS/CACHE/GAMES.IDX'); self.assertEqual(r, 0)
        self.assertEqual(self.open('/sd/OS/SAVES/DICE/NEW.HIS', 0x601)[0], -16)
        self.assertEqual(self.c.fatsave_put(b'DICE', b'DICE.HIS', b'z', 1), -16)
        self.assertEqual(self.close(rd), 0)
        r, rd = self.open('/sd/OS/SAVES/DICE/DICE.HIS'); self.assertEqual(r, 0)
        self.assertEqual(self.open('/sd/OS/CACHE/NEW.IDX', 0x601)[0], -16)
        self.assertEqual(self.c.fatcache_put(b'GAMES.IDX', b'z', 1), -16)
        self.assertEqual(self.close(rd), 0)
        self.assertEqual(self.open('/sd/OS/CACHE/NEW.IDX', 0x601)[0], 0)

if __name__ == '__main__': unittest.main()
