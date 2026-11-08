# SPDX-License-Identifier: MIT
"""Checked call20 batching; host read counts, not physical SD timings."""
import ctypes as C
import os
import struct
import fatgen
from test_m1_user import CallsBase, program, peek


class GetdentsScan(CallsBase):
    def card(self, extra=None, order=0, fb=True):
        self.disk = self.work / f'batch{id(self)}.img'
        self.vol = fatgen.build(self.disk, {
            'OS': {'DIAG.ELF': program()},
            'GAMES': {f'F{i:07}.DAT': b'' for i in range(100)},
            'OTHER': {'OTHER.DAT': b'payload'}}, spc=2, fragment=True)
        self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.c.asset_mount(), 0)
        self.c.fb_setup(order, fb)
        self.assertFalse(self.c.os_start(b'/OS/DIAG.ELF'))

    def names(self, data, metadata=True):
        out = []
        at = 0
        while at < len(data):
            size, typ, nb = struct.unpack_from('<HBB', data, at)
            out.append(data[at+(24 if metadata else 8):at+(24 if metadata else 8)+nb].decode())
            self.assertIn(typ, (1, 2))
            self.assertEqual(size % 8, 0)
            at += size
        self.assertEqual(at, len(data))
        return out

    def listing(self, call, fd, cap=4096):
        status, n = self.io(call, fd, self.BUF, cap)
        self.assertEqual(status, 0)
        return self.names(peek(self.c, self.BUF, n), call != 11)

    def edit(self, name, replacement):
        # Drop libc read-ahead, but preserve kernel handles/cursors.
        self.c.close_disk()
        fatgen.patch(self.disk, self.vol.entry_offset('/GAMES/'+name), replacement)
        self.c.open_disk(os.fsencode(self.disk))

    def test_call20_reads_directory_sectors_once_per_batch(self):
        r, fd = self.open('/sd/GAMES'); self.assertEqual(r, 0)
        before = self.c.read_count()
        self.assertEqual(self.listing(20, fd), [f'F{i:07}.DAT' for i in range(100)])
        self.assertLessEqual(self.c.read_count()-before, 12)
        self.assertEqual(self.close(fd), 0)

    def test_legacy_calls_keep_entry_local_reads_and_wire_layout(self):
        for call in (11, 18):
            with self.subTest(call=call):
                r, fd = self.open('/sd/GAMES'); self.assertEqual(r, 0)
                before = self.c.read_count()
                self.assertEqual(self.listing(call, fd), [f'F{i:07}.DAT' for i in range(100)])
                self.assertGreaterEqual(self.c.read_count()-before, 100)
                self.close(fd)

    def test_boundary_edit_interleaving_and_pending_entry(self):
        r, a = self.open('/sd/GAMES'); self.assertEqual(r, 0)
        r, b = self.open('/sd/OTHER'); self.assertEqual(r, 0)
        self.assertEqual(self.listing(20, a, 40), ['F0000000.DAT'])
        self.assertEqual(self.listing(20, b), ['OTHER.DAT'])
        self.edit('F0000002.DAT', b'CHANGED ')
        names = self.listing(20, a)
        self.assertEqual(names[:2], ['F0000001.DAT', 'CHANGED.DAT'])
        self.assertEqual(len(names), 99)
        self.close(a); self.close(b)

    def test_invalid_buffer_and_small_buffer_do_not_leak_sector_bytes(self):
        r, fd = self.open('/sd/GAMES'); self.assertEqual(r, 0)
        self.assertEqual(self.io(20, fd, 0, 4096)[0], -14)
        self.assertEqual(self.io(20, fd, self.BUF, 8)[0], -22)
        self.edit('F0000001.DAT', b'FRESH   ')
        names = self.listing(20, fd)
        self.assertEqual(names[:2], ['F0000000.DAT', 'FRESH.DAT'])
        self.close(fd)

    def test_first_sector_failure_retries_fresh_bytes(self):
        r, fd = self.open('/sd/GAMES'); self.assertEqual(r, 0)
        self.c.fail_read_sector(self.vol.lba(self.vol.chains['/GAMES'][0]))
        self.assertEqual(self.io(20, fd, self.BUF, 4096)[0], -5)
        self.c.fail_read_sector(0xffffffff)
        self.edit('F0000000.DAT', b'RETRIED ')
        self.assertEqual(self.listing(20, fd)[0], 'RETRIED.DAT')
        self.close(fd)

    def test_partial_batch_failure_retries_same_cursor_and_discards_cache(self):
        r, fd = self.open('/sd/GAMES'); self.assertEqual(r, 0)
        # Two dot slots leave 14 files in the first directory sector.
        self.c.fail_read_sector(self.vol.lba(self.vol.chains['/GAMES'][0])+1)
        first = self.listing(20, fd)
        self.assertEqual(first, [f'F{i:07}.DAT' for i in range(14)])
        self.assertEqual(self.io(20, fd, self.BUF, 4096)[0], -5)
        self.c.fail_read_sector(0xffffffff)
        self.edit('F0000014.DAT', b'RETRIED ')
        rest = self.listing(20, fd)
        self.assertEqual(rest, ['RETRIED.DAT']+[f'F{i:07}.DAT' for i in range(15,100)])
        self.close(fd)
