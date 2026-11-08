# SPDX-License-Identifier: MIT
"""Real FAT cache-only storage, not POSIX rename emulation."""
import ctypes as C
import unittest
from test_m1_user import InstallBase, text

def sparse_copy(src, dst):
    # Portable to macOS too; never require GNU cp flags in the host suite.
    with open(src, 'rb') as a, open(dst, 'wb') as b:
        a.seek(0, 2); size = a.tell(); a.seek(0); b.truncate(size)
        for offset in range(0, size, 1 << 20):
            data = a.read(1 << 20)
            if data.strip(b'\0'): b.seek(offset); b.write(data)

class CacheStorage(InstallBase):
    def put_cache(self, name, data):
        self.assertTrue(hasattr(self.c, 'fatcache_put'), 'cache-only COW storage is missing')
        self.c.fatcache_put.argtypes = [C.c_char_p, C.c_char_p, C.c_uint32]
        return self.c.fatcache_put(name, data, len(data))

    def test_create_and_replace_cache_preserves_boot_files(self):
        self.assertEqual(self.put_cache(b'GAMES.TMP', b'first'), 0)
        self.assertEqual(self.read('/OS/CACHE/GAMES.TMP'), b'first')
        self.assertEqual(self.put_cache(b'GAMES.TMP', b'second' * 1000), 0)
        self.assertEqual(self.read('/OS/CACHE/GAMES.TMP'), b'second' * 1000)
        self.untouched()

    def test_rename_replaces_atomically_and_removes_temp(self):
        self.assertEqual(self.put_cache(b'GAMES.IDX', b'old'), 0)
        self.assertEqual(self.put_cache(b'GAMES.TMP', b'new' * 500), 0)
        self.assertTrue(hasattr(self.c, 'fatcache_rename'), 'cache rename is missing')
        self.c.fatcache_rename.argtypes = [C.c_char_p, C.c_char_p, C.c_void_p, C.c_uint32]
        buf = C.create_string_buffer(1 << 20)
        self.assertEqual(self.c.fatcache_rename(b'GAMES.TMP', b'GAMES.IDX', buf, len(buf)), 0)
        self.assertEqual(self.read('/OS/CACHE/GAMES.IDX'), b'new' * 500)
        self.assertEqual(self.c.asset_open(b'/OS/CACHE/GAMES.TMP', 19, 0), -2)
        self.untouched()

    def test_installs_unpublish_cache_before_same_size_fixed_date_replacement(self):
        self.assertEqual(self.c.fatput(b'GAMES/DICE/DICE.GAM', b'old', 3), 0)
        self.assertEqual(self.put_cache(b'GAMES.IDX', b'cached'), 0)
        self.assertEqual(self.c.fatput(b'GAMES/DICE/DICE.GAM', b'new', 3), 0)
        self.assertEqual(self.c.asset_open(b'/OS/CACHE/GAMES.IDX', 19, 0), -2,
                         'USB installs must invalidate before content publication')
        self.assertEqual(self.read('/GAMES/DICE/DICE.GAM'), b'new')

    def test_rename_fsinfo_is_consistent(self):
        from test_m1_user import fsck
        self.assertEqual(self.put_cache(b'GAMES.TMP', b'data' * 1000), 0)
        buf = C.create_string_buffer(1 << 20)
        self.assertEqual(self.c.fatcache_rename(b'GAMES.TMP', b'GAMES.IDX', buf, len(buf)), 0)
        r = fsck(self.disk, self.work)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn('wrong', r.stdout.lower())

    def test_every_rename_write_cut_remounts_to_old_or_new_without_crosslinks(self):
        import os
        old, new = b'old' * 500, b'new' * 500
        self.assertEqual(self.put_cache(b'GAMES.IDX', old), 0)
        self.assertEqual(self.put_cache(b'GAMES.TMP', new), 0)
        baseline = self.work / 'rename-baseline.img'
        sparse_copy(self.disk, baseline)
        buf = C.create_string_buffer(1 << 20)
        start = self.c.write_count()
        self.assertEqual(self.c.fatcache_rename(b'GAMES.TMP', b'GAMES.IDX', buf, len(buf)), 0)
        steps = self.c.write_count() - start
        self.assertGreater(steps, 5)
        for cut in range(steps + 1):
            with self.subTest(cut=cut):
                self.c.close_disk()
                sparse_copy(baseline, self.disk)
                self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.c.inject_failure(self.c.write_count() + cut)
                result = self.c.fatcache_rename(b'GAMES.TMP', b'GAMES.IDX', buf, len(buf))
                self.assertEqual(result == 0, cut == steps)
                self.c.inject_failure(0xffffffff)
                self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.assertIn(self.read('/OS/CACHE/GAMES.IDX'), (old, new))
                for name, data in self.before.items(): self.assertEqual(self.read('/' + name), data)
                # A remaining temp must have an independent chain, never point
                # at published destination data (even before a later fsck).
                h = self.c.asset_open(b'/OS/CACHE', 9, 0)
                from test_m1 import Dirent
                d = Dirent(); meta = (C.c_uint32 * 3)(); first = {}
                while self.c.asset_readdir_meta(h, C.byref(d), meta) > 0:
                    first[bytes(d.name).split(b'\0')[0]] = meta[2]
                self.c.asset_close(h)
                def clusters(c):
                    seen = set(); fat = self.fats()[0]
                    while c < 0x0ffffff8:
                        self.assertGreaterEqual(c, 2); self.assertNotIn(c, seen)
                        seen.add(c); c = struct.unpack_from('<I', fat, c * 4)[0] & 0x0fffffff
                    return seen
                if b'GAMES.TMP' in first: self.assertFalse(clusters(first[b'GAMES.TMP']) & clusters(first[b'GAMES.IDX']))
        print('cache rename write-cut points:', steps + 1)

    def test_every_initial_create_write_cut_leaves_complete_cache_or_missing(self):
        import os
        data = b'new cache' * 200
        baseline = self.work / 'create-baseline.img'; sparse_copy(self.disk, baseline)
        start = self.c.write_count()
        self.assertEqual(self.put_cache(b'GAMES.IDX', data), 0)
        steps = self.c.write_count() - start
        for cut in range(steps + 1):
            with self.subTest(cut=cut):
                self.c.close_disk(); sparse_copy(baseline, self.disk)
                self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.c.inject_failure(self.c.write_count() + cut)
                result = self.put_cache(b'GAMES.IDX', data)
                self.assertEqual(result == 0, cut == steps)
                self.c.inject_failure(0xffffffff)
                self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                h = self.c.asset_open(b'/OS/CACHE/GAMES.IDX', 19, 0)
                if h >= 0:
                    buf = C.create_string_buffer(len(data) + 1)
                    self.assertEqual(self.c.asset_read(h, buf, len(buf)), len(data))
                    self.assertEqual(buf.raw[:len(data)], data); self.c.asset_close(h)
                else: self.assertEqual(h, -2)
                for name, expected in self.before.items(): self.assertEqual(self.read('/' + name), expected)
        print('cache initial-create write-cut points:', steps + 1)

    def test_cache_cannot_use_forged_fsinfo_to_modify_noncache_file(self):
        import os, struct
        from test_m1_user import boot_card
        from test_m1 import Dirent
        trap = bytearray(512)
        struct.pack_into('<I', trap, 0, 0x41615252)
        struct.pack_into('<I', trap, 484, 0x61417272)
        self.c.close_disk()
        self.disk, self.folder = boot_card(self.work, {'TRAP.BIN': bytes(trap)})
        self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
        volume = (C.c_uint32 * 8)(); self.assertEqual(self.c.asset_volume(volume), 0)
        h = self.c.asset_open(b'/', 1, 0); d = Dirent(); meta = (C.c_uint32 * 3)()
        first = 0
        while self.c.asset_readdir_meta(h, C.byref(d), meta) > 0:
            if bytes(d.name) == b'TRAP.BIN': first = meta[2]
        self.c.asset_close(h); self.assertGreaterEqual(first, 2)
        sector = volume[4] + (first - 2) * volume[6]
        with open(self.disk, 'r+b') as f:
            f.seek(volume[0] * 512 + 48); f.write(struct.pack('<H', sector - volume[0]))
        self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
        self.assertLess(self.put_cache(b'GAMES.IDX', b'new'), 0, 'forged FSInfo must fail before cache mutation')
        self.assertEqual(self.read('/TRAP.BIN'), bytes(trap))

    def test_readonly_or_oversized_cache_cannot_block_usb_installs(self):
        import mmap, os
        for readonly in (True, False):
            with self.subTest(readonly=readonly):
                self.assertEqual(self.put_cache(b'GAMES.IDX', b'cached'), 0)
                with open(self.disk, 'r+b') as f, mmap.mmap(f.fileno(), 0) as m:
                    at = m.find(b'GAMES   IDX'); self.assertGreater(at, 0)
                    if readonly: m[at+11] |= 1
                    else: m[at+28:at+32] = struct.pack('<I', (1 << 20) + 1)
                self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
                self.assertEqual(self.c.fatput(b'GAMES/DICE/DICE.GAM', b'new', 3), 0,
                                 'expendable cache must not deny unrelated installs')
                self.assertEqual(self.c.asset_open(b'/OS/CACHE/GAMES.IDX', 19, 0), -2)
                self.assertEqual(self.read('/GAMES/DICE/DICE.GAM'), b'new')

    def test_oversized_cache_entry_can_be_rebuilt(self):
        import mmap, os
        self.assertEqual(self.put_cache(b'GAMES.IDX', b'cached'), 0)
        with open(self.disk, 'r+b') as f, mmap.mmap(f.fileno(), 0) as m:
            at = m.find(b'GAMES   IDX'); self.assertGreater(at, 0)
            m[at+28:at+32] = struct.pack('<I', (1 << 20) + 1)
        self.c.close_disk(); self.c.open_disk(os.fsencode(self.disk)); self.assertEqual(self.c.asset_mount(), 0)
        self.assertEqual(self.put_cache(b'GAMES.IDX', b'rebuilt'), 0)
        self.assertEqual(self.read('/OS/CACHE/GAMES.IDX'), b'rebuilt')

from test_m1_user import CallsBase, poke, peek
import struct

class CacheCalls(CallsBase):
    def test_explicit_sync_create_and_rename(self):
        r, fd = self.open('/sd/OS/CACHE/GAMES.TMP', 1 | 0x200 | 0x400)
        self.assertEqual(r, 0, 'scoped cache create is missing')
        poke(self.c, self.BUF, b'cache bytes')
        self.assertEqual(self.io(3, fd, self.BUF, 11), (0, 11))
        self.assertEqual(self.call(12, self.hdr(24) + struct.pack('<Q', fd)), 0)
        self.assertEqual(self.close(fd), 0)
        a, b = b'/sd/OS/CACHE/GAMES.TMP', b'/sd/OS/CACHE/GAMES.IDX'
        poke(self.c, self.BUF, a); poke(self.c, self.BUF + 128, b)
        self.assertEqual(self.call(19, self.hdr(48) + struct.pack('<4Q', self.BUF, len(a), self.BUF + 128, len(b))), 0)
        r, fd = self.open(b); self.assertEqual(r, 0)
        self.assertEqual(self.io(2, fd, self.BUF, 100), (0, 11))
        self.assertEqual(peek(self.c, self.BUF, 11), b'cache bytes')

    def test_metadata_getdents_retains_legacy_layout(self):
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        self.assertEqual(self.io(18, fd, self.BUF, 512)[0], 0, 'metadata enumeration is missing')
        data = peek(self.c, self.BUF, 512)
        size, typ, nb, nbytes, mtime, ctime, cluster, reserved = struct.unpack_from('<HBB5I', data)
        self.assertGreaterEqual(size, 24 + nb)
        self.assertEqual(reserved, 0)
        self.assertEqual(data[24:24 + nb], b'MIT.TXT')
        self.assertEqual(nbytes, len((__import__('pathlib').Path(__file__).resolve().parents[1] / 'LICENSE').read_bytes()))
        self.assertGreaterEqual(cluster, 2)
        self.assertEqual(self.close(fd), 0)
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        self.assertEqual(self.io(11, fd, self.BUF, 512)[0], 0)
        self.assertEqual(peek(self.c, self.BUF + 8, 7), b'MIT.TXT')

    def test_cache_scope_and_embedded_nul_are_rejected(self):
        for name in ('/sd/OS/OTHER.IDX', '/sd/GAMES/X', '/sd/OS/CACHE/../X', '/sd/OS/CACHE/A/B',
                     '/sd/os/cache/X', '/sd/OS/CACHE/TOOLONGNAME.X', '/dev/log'):
            with self.subTest(name): self.assertLess(self.open(name, 0x601)[0], 0)
        a = b'/sd/OS/CACHE/A\x00JUNK'; b = b'/sd/OS/CACHE/B'
        poke(self.c, self.BUF, a); poke(self.c, self.BUF + 128, b)
        self.assertEqual(self.call(19, self.hdr(48) + struct.pack('<4Q', self.BUF, len(a), self.BUF + 128, len(b))), -22)

    def test_close_without_sync_aborts_and_stale_writer_is_rejected(self):
        r, fd = self.open('/sd/OS/CACHE/ABORT.TMP', 0x601); self.assertEqual(r, 0)
        poke(self.c, self.BUF, b'uncommitted')
        self.assertEqual(self.io(3, fd, self.BUF, 11), (0, 11))
        self.assertEqual(self.open('/sd/OS/CACHE/OTHER.TMP', 0x601)[0], -16)
        self.assertEqual(self.close(fd), 0)
        self.assertEqual(self.open('/sd/OS/CACHE/ABORT.TMP')[0], -2)
        self.assertEqual(self.call(12, self.hdr(24) + struct.pack('<Q', fd)), -9)

    def test_metadata_pending_can_switch_record_formats(self):
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        self.assertEqual(self.io(18, fd, self.BUF, 31)[0], -22)
        self.assertEqual(self.io(11, fd, self.BUF, 24), (0, 16))
        self.assertEqual(peek(self.c, self.BUF + 8, 7), b'MIT.TXT')
        self.assertEqual(self.io(18, fd, self.BUF, 32), (0, 32))
        fields = struct.unpack('<HBB5I', peek(self.c, self.BUF, 24))
        self.assertEqual(fields[1:3], (2, 2))
        self.assertGreaterEqual(fields[6], 2)
        self.assertEqual(peek(self.c, self.BUF + 24, 2), b'OS')

    def test_cache_reader_alias_blocks_mutation_and_writer_quota_is_atomic(self):
        from test_m1_user import IMAGE, R, W
        r, fd = self.open('/sd/OS/CACHE/FULL.TMP', 0x601); self.assertEqual(r, 0)
        base = IMAGE + 0x4000
        self.assertEqual(self.c.us_map(base, 1 << 20, R | W), 0)
        self.assertEqual(self.io(3, fd, base, 1 << 20), (0, 1 << 20))
        self.assertEqual(self.io(3, fd, base, 1)[0], -28)
        self.assertEqual(self.call(12, self.hdr(24) + struct.pack('<Q', fd)), 0)
        self.assertEqual(self.close(fd), 0)
        r, reader = self.open('/sd/os/cache/full.tmp'); self.assertEqual(r, 0)
        self.assertEqual(self.open('/sd/OS/CACHE/NEW.TMP', 0x601)[0], -16)
        self.assertEqual(self.io(2, reader, base, 1 << 20), (0, 1 << 20))
        self.assertEqual(self.io(2, reader, base, 1), (0, 0))
        self.assertEqual(self.close(reader), 0)
        self.assertEqual(self.open('/sd/OS/CACHE/NEW.TMP', 0x601)[0], 0)

    def test_new_call_record_envelopes_reject_before_media_mutation(self):
        before = self.c.write_count()
        for call, size in ((12, 24), (18, 48), (19, 48)):
            for flag_offset in (4, 8, 12):
                rec = bytearray(self.hdr(size) + bytes(size - 16)); struct.pack_into('<I', rec, flag_offset, 1)
                self.assertEqual(self.call(call, bytes(rec)), -22)
            rec = self.hdr(size + 8) + bytes(size - 16) + b'\1' + bytes(7)
            self.assertEqual(self.call(call, rec), -22)
            self.assertEqual(self.call(call, self.hdr(size) + bytes(size - 16), major=2), -93)
        self.assertEqual(self.c.write_count(), before)

if __name__ == '__main__': unittest.main()
