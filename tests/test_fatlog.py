# SPDX-License-Identifier: MIT
"""Bounded in-place logs (fatlog): one sector written per pump, text round-trips, a cut never harms the FAT."""
import ctypes as C
import os
from test_m1_user import InstallBase, fsck
from test_cache import sparse_copy

CEC, AUDIO = 0, 1
LOG = '/OS/SAVES/KERNEL/CEC.TXT'

class FatLog(InstallBase):
    def setUp(self):
        super().setUp()
        self.c.fatlog_set.argtypes = [C.c_uint, C.c_char_p, C.c_uint32]
        self.c.fatlog_pending.restype = C.c_bool

    def set(self, data, slot=CEC): self.assertEqual(self.c.fatlog_set(slot, data, len(data)), 0)

    def drain(self):
        """Pump until idle; returns the sector writes of each pass (each pass at most one data sector)."""
        passes = []
        for _ in range(200):
            w = self.c.write_count(); r = self.c.fatlog_pump(); self.assertGreaterEqual(r, 0)
            if r == 0: break
            passes.append(self.c.write_count() - w)
        return passes

    def lines(self, n, tag=b'line'): return b''.join(b'%08d %s %d\n' % (i * 7, tag, i) for i in range(n))

    def test_round_trip_24k_one_sector_per_pass_and_plain_text(self):
        text = self.lines(1000)[:24000]
        self.set(text)
        self.assertEqual(self.c.fatlog_pump(), 1)          # may create the file; still one data sector written
        passes = self.drain()
        self.assertTrue(all(p <= 2 for p in passes), passes)  # a sector and its read-back: 1 write + 1 read, never more than 1 write
        got = self.read(LOG)
        self.assertEqual(len(got), 24576)
        self.assertEqual(got.rstrip(b'\0'), text)
        self.assertEqual(got[len(text):], b'\0' * (24576 - len(text)))
        r = fsck(self.disk, self.work); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_append_rewrites_only_changed_sectors_and_never_the_fat(self):
        base = self.lines(300); self.set(base); self.drain()
        a0, b0 = self.fats(); w = self.c.fatlog_writes()
        self.set(base + b'tail one\n'); self.drain()
        self.assertEqual(self.c.fatlog_writes() - w, 1)     # the tail sits inside one already-written sector
        self.assertEqual(self.fats(), (a0, b0))
        self.assertEqual(self.read(LOG).rstrip(b'\0'), base + b'tail one\n')
        self.set(base + b'tail one\n'); w = self.c.fatlog_writes(); self.assertEqual(self.c.fatlog_pump(), 0)
        self.assertEqual(self.c.fatlog_writes(), w)         # nothing changed, nothing written

    def test_shifted_log_converges_and_other_files_are_untouched(self):
        self.set(self.lines(900)[:24000]); self.drain()
        new = self.lines(900, b'next')[:24000]; self.set(new); self.drain()
        self.assertEqual(self.read(LOG).rstrip(b'\0'), new)
        self.untouched_data = {n: self.read('/' + n) for n in self.before}
        for n, d in self.before.items(): self.assertEqual(self.untouched_data[n], d)

    def test_audio_slot_is_8704_bytes_and_rotation_copies_still_fit(self):
        t = b'AUDIO v2 boot=1\n' + b'x' * 8000; self.set(t, AUDIO); self.drain()
        got = self.read('/OS/SAVES/KERNEL/AUDIO.TXT'); self.assertEqual(len(got), 8704); self.assertEqual(got.rstrip(b'\0'), t)
        buf = C.create_string_buffer(8704); self.c.fatsave_rename.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_void_p, C.c_uint32]
        self.assertEqual(self.c.fatsave_rename(b'KERNEL', b'AUDIO.TXT', b'AUDIO.1', buf, 8704), 0)
        self.assertEqual(self.read('/OS/SAVES/KERNEL/AUDIO.1').rstrip(b'\0'), t)
        self.set(b'new\n', AUDIO); self.drain()      # the next boot's file is created again
        self.assertEqual(self.read('/OS/SAVES/KERNEL/AUDIO.TXT').rstrip(b'\0'), b'new\n')

    def test_every_write_cut_keeps_fat_intact_and_earlier_lines_readable(self):
        old = self.lines(400); self.set(old); self.drain()
        baseline = self.work / 'log-baseline.img'; sparse_copy(self.disk, baseline)
        new = old + self.lines(40, b'more')
        self.set(new); start = self.c.write_count(); self.drain(); steps = self.c.write_count() - start
        self.assertGreater(steps, 2)
        for cut in range(steps + 1):
            with self.subTest(cut=cut):
                self.c.close_disk(); sparse_copy(baseline, self.disk)
                self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.set(old); self.drain()                    # a fresh boot: known state, nothing pending
                self.set(new); self.c.inject_failure(self.c.write_count() + cut)
                for _ in range(100): self.c.fatlog_pump()
                self.c.inject_failure(0xffffffff)
                self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                got = self.read(LOG).rstrip(b'\0')
                self.assertTrue(got.startswith(old[:1024]), 'earlier lines readable')
                self.assertTrue(len(got) >= len(old))
                for n, d in self.before.items(): self.assertEqual(self.read('/' + n), d)
                r = fsck(self.disk, self.work); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def entry(self, name11):
        """Offset of a directory entry's 32 bytes in the disk image (the first whole-name match)."""
        self.c.close_disk(); self.addCleanup(lambda: None)
        with open(self.disk, 'rb') as f: i = f.read().find(name11)
        self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
        self.assertGreater(i, 0); return i

    def patch(self, off, b):
        self.c.close_disk()
        with open(self.disk, 'r+b') as f: f.seek(off); f.write(b)
        self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)

    def test_replaced_log_never_lets_the_pump_write_into_another_files_clusters(self):
        self.set(self.lines(900)[:24000]); self.drain()
        self.c.fatsave_put.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint32]
        self.assertEqual(self.c.fatsave_put(b'KERNEL', b'CEC.TXT', b'small\n', 6), 0)   # an app's save replaces the log's clusters
        other = bytes((i * 7 + 3) & 255 for i in range(24576))                        # a new file takes the freed clusters
        self.assertEqual(self.c.fatput(b'GAMES/DICE/DICE.BIN', other, len(other)), 0)
        self.assertEqual(self.read('/GAMES/DICE/DICE.BIN'), other)
        self.set(self.lines(50, b'after')); self.drain()
        self.assertEqual(self.read('/GAMES/DICE/DICE.BIN'), other)
        self.assertEqual(self.read(LOG).rstrip(b'\0'), self.lines(50, b'after'))
        r = fsck(self.disk, self.work); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_a_changed_directory_entry_is_noticed_even_without_a_forget_call(self):
        self.set(self.lines(900)[:24000]); self.drain()
        off = self.entry(b'CEC     TXT')
        self.patch(off + 28, b'\0\0\0\0')                     # another writer changed the size behind the pump's back
        self.set(self.lines(30, b'again')); w = self.c.write_count()
        self.assertEqual(self.c.fatlog_pump(), 1)
        self.drain()
        self.assertEqual(self.read(LOG).rstrip(b'\0'), self.lines(30, b'again'))   # (the leaked old chain is why there is no fsck here)

    def test_a_chain_shared_with_another_name_is_refused(self):
        other = bytes((i * 5 + 1) & 255 for i in range(24576))
        self.assertEqual(self.c.fatput(b'GAMES/DICE/DICE.BIN', other, len(other)), 0)
        self.set(self.lines(10)); self.drain()
        with open(self.disk, 'rb') as f: data = f.read()
        a = data.find(b'CEC     TXT'); b = data.find(b'DICE    BIN'); self.assertGreater(a, 0); self.assertGreater(b, 0)
        self.patch(b + 20, data[a + 20:a + 22]); self.patch(b + 26, data[a + 26:a + 28])   # DICE.BIN now starts on the log's chain
        before = self.read('/GAMES/DICE/DICE.BIN'); self.c.fatlog_forget()
        self.set(self.lines(20, b'zzz')); res = [self.c.fatlog_pump() for _ in range(5)]
        self.assertTrue(any(r < 0 for r in res), res)
        self.assertEqual(self.read('/GAMES/DICE/DICE.BIN'), before)
