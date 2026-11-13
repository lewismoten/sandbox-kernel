# SPDX-License-Identifier: MIT
"""Bounded in-place logs (fatlog): one sector written per pump, text round-trips, a cut never harms the FAT."""
import ctypes as C
import os
import struct
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
        self.set(self.lines(20, b'zzz')); res = [self.c.fatlog_pump() for _ in range(400)]
        self.assertTrue(any(r < 0 for r in res), res)
        self.assertEqual(self.read('/GAMES/DICE/DICE.BIN'), before)

    def test_creation_is_given_up_after_three_failures_for_the_boot(self):
        self.c.fatsave_put.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint32]
        for i in range(32): self.assertEqual(self.c.fatsave_put(b'KERNEL', b'N%d.TXT' % i, b'x', 1), 0)   # the folder is at its file cap
        self.set(self.lines(20)); errors = 0
        for _ in range(60000): errors += self.c.fatlog_pump() < 0
        self.assertEqual(errors, 1, 'one report, then silence')
        w = self.c.write_count(); r0 = self.c.read_count()
        self.set(self.lines(30)); self.assertEqual(self.c.fatlog_pump(), 0)
        self.assertEqual((self.c.write_count(), self.c.read_count()), (w, r0), 'no more card work this boot')

    def test_a_chain_freed_and_reused_from_the_same_first_cluster_is_noticed(self):
        self.set(self.lines(900)[:24000]); self.drain()
        with open(self.disk, 'rb') as f: data = f.read()
        marker = data.find(b'line 899'); a = data.find(b'CEC     TXT'); self.assertGreater(marker, 0)
        first = struct.unpack('<H', data[a + 20:a + 22])[0] << 16 | struct.unpack('<H', data[a + 26:a + 28])[0]
        for copy in (0, 1): self.patch((2048 + 32 + copy * 2048) * 512 + first * 4, struct.pack('<I', 0x0fffffff))   # chain now ends at its first cluster
        self.set(self.lines(30, b'again')); self.drain()
        self.assertEqual(self.read(LOG).rstrip(b'\0'), self.lines(30, b'again'))
        with open(self.disk, 'rb') as f: self.assertEqual(f.read()[marker:marker + 8], b'line 899')   # the old later clusters were not written

    def ops(self): return self.c.read_count() + self.c.write_count()

    def test_every_pass_is_bounded_even_with_many_game_folders_and_a_set_per_pass(self):
        """The 1.35 s stall: a whole-zone directory walk and a 24 KB zero-filled create in ONE pass, repeated after every set.
        Now every pass is a bounded step (counted in card sector operations; a host test has no real clock)."""
        for i in range(40): self.assertEqual(self.c.fatput(b'GAMES/G%02d/G%02d.BIN' % (i, i), b'x' * 700, 700), 0)
        self.c.fatsave_put.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint32]
        self.assertEqual(self.c.fatsave_put(b'KERNEL', b'KEEP.TXT', b'x', 1), 0)   # folder creation is put_file's one-time job (it scans the FAT from cluster 2)
        worst = 0; passes = 0
        self.set(self.lines(900)[:24000])
        for rnd in range(6):                           # creation, then appends that each call set again
            for _ in range(3000):
                a = self.ops(); r = self.c.fatlog_pump(); worst = max(worst, self.ops() - a); passes += 1
                self.assertGreaterEqual(r, 0)
                if r == 0: break
            self.set(self.lines(900 + rnd * 3)[:24000])
        self.assertLessEqual(worst, 12, 'card sector operations in the worst pass')
        self.assertGreater(passes, 40)

    def test_an_append_after_the_file_is_open_costs_no_directory_walk(self):
        for i in range(40): self.assertEqual(self.c.fatput(b'GAMES/G%02d/G%02d.BIN' % (i, i), b'x' * 700, 700), 0)
        base = self.lines(300); self.set(base); self.drain()
        r0 = self.c.read_count(); self.set(base + b'more\n'); self.drain()
        self.assertLessEqual(self.c.read_count() - r0, 8, 'one entry check and one read-back, no zone walk')

    def test_never_writes_while_an_os_syscall_holds_the_card(self):
        busy = C.c_uint32.in_dll(self.c, 'card_busy'); self.set(self.lines(100)); busy.value = 1
        w, r0 = self.c.write_count(), self.c.read_count()
        for _ in range(50): self.assertEqual(self.c.fatlog_pump(), 0)
        self.assertEqual((self.c.write_count(), self.c.read_count()), (w, r0)); busy.value = 0
        self.drain(); self.assertEqual(self.read(LOG).rstrip(b'\0'), self.lines(100))

    def test_a_failed_write_anywhere_in_a_creation_leaves_no_orphan_and_the_log_still_arrives(self):
        """One transient write failure at every write of a creation: the rollback frees whatever was linked, so fsck finds no
        lost cluster chain right after the failure, and the next attempt still produces the whole log."""
        self.c.fatsave_put.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint32]
        self.assertEqual(self.c.fatsave_put(b'KERNEL', b'KEEP.TXT', b'x', 1), 0)   # the folders exist (their creation is put_file's, not the log's)
        baseline = self.work / 'creation-baseline.img'; sparse_copy(self.disk, baseline)
        text = self.lines(900)[:24000]
        self.set(text); start = self.c.write_count(); self.drain(); steps = self.c.write_count() - start
        self.assertGreater(steps, 30)
        for cut in range(steps):
            with self.subTest(failed_write=cut):
                self.c.close_disk(); sparse_copy(baseline, self.disk)
                self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.c.fatlog_forget(); self.set(text); self.c.fail_write_once(cut)
                errs = 0
                for _ in range(3000):
                    r = self.c.fatlog_pump(); errs += r < 0
                    if not self.c.fatlog_pending(CEC): break
                self.assertEqual(errs, 0, 'one transient failure is retried quietly (only the third in a row is reported)')
                self.assertFalse(self.c.fatlog_pending(CEC), 'the retry finished')
                self.assertEqual(self.read(LOG).rstrip(b'\0'), text)
                r = fsck(self.disk, self.work); self.assertEqual(r.returncode, 0, (r.stdout + r.stderr)[-400:])
