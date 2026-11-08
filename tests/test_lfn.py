# SPDX-License-Identifier: MIT
"""VFAT names through the real reader; short APIs must remain ABI compatible."""
import ctypes as C
import os
import pathlib
import shutil
import tempfile
import unittest
from typing import Any
import fatlfn
import hostlib

class ShortEntry(C.Structure):
    _fields_ = [('name', C.c_char*13), ('attr', C.c_uint8),
                ('is_dir', C.c_uint8), ('reserved', C.c_uint8), ('size', C.c_uint32)]

class LongEntry(C.Structure):
    _fields_ = [('name', C.c_char*256), ('short_entry', ShortEntry)]

class LongNames(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        cls.lib = hostlib.build(cls.work/'host.so')

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def setUp(self):
        case = pathlib.Path(tempfile.mkdtemp(dir=self.work))
        shutil.copyfile(self.lib, case/'host.so')
        self.c = C.CDLL(str(case/'host.so'))
        self.c.open_disk.argtypes = [C.c_char_p]
        self.c.asset_open.argtypes = [C.c_char_p, C.c_uint32, C.c_uint32]
        self.c.asset_read.argtypes = [C.c_int, C.c_void_p, C.c_uint32]
        self.card = case/'card.img'
        self.tree: dict[str, Any] = {'GAMES': {'LONGDI~1': {'LONGFI~1.GAM': b'game payload'*123}}}
        self.names = {'/GAMES/LONGDI~1': 'My dragged game',
                      '/GAMES/LONGDI~1/LONGFI~1.GAM': 'A long game name.gam'}

    def mount(self):
        self.vol = fatlfn.build(self.card, self.tree, self.names)
        self.c.open_disk(os.fsencode(self.card))
        self.addCleanup(self.c.close_disk)
        self.assertEqual(self.c.asset_mount(), 0)

    def open(self, path):
        b = path.encode() if isinstance(path, str) else path
        return self.c.asset_open(b, len(b), 0)

    def read(self, path):
        h = self.open(path)
        self.assertGreaterEqual(h, 0, path)
        data, out = C.create_string_buffer(4096), bytearray()
        while (n := self.c.asset_read(h, data, len(data))) > 0:
            out += data.raw[:n]
        self.assertEqual(n, 0)
        self.assertEqual(self.c.asset_close(h), 0)
        return bytes(out)

    def long_listing(self, path):
        self.c.asset_readdir_long.argtypes = [C.c_int, C.POINTER(LongEntry), C.POINTER(C.c_uint32)]
        h = self.open(path); self.assertGreaterEqual(h, 0, path)
        e, meta, out = LongEntry(), (C.c_uint32*3)(), []
        while (r := self.c.asset_readdir_long(h, C.byref(e), meta)) == 1:
            out.append((e.name.decode(), e.short_entry.name.decode('latin-1')))
        self.assertEqual(r, 0); self.c.asset_close(h)
        return out

    def edit(self, offset, data):
        self.c.close_disk()
        import fatgen
        fatgen.patch(self.card, offset, data)
        self.c.open_disk(os.fsencode(self.card))

    def test_malformed_sequences_fall_back_to_short_alias(self):
        self.mount()
        short = self.vol.entry_offset('/GAMES/LONGDI~1')
        with open(self.card, 'rb') as f:
            f.seek(short-64); original = f.read(64)
        # Two LFN slots: LAST|2 then 1. Never attach malformed fragments.
        changes = [
            (0, b'\x42', 'valid control'),
            (0, b'\x02', 'missing LAST'), (0, b'\x40', 'zero ordinal'),
            (0, b'\x55', 'ordinal 21'), (0, b'\xc2', 'reserved ordinal bit'),
            (0, b'\x62', 'reserved ordinal bit 5'),
            (11, b'\x4f', 'reserved attribute'), (12, b'\x01', 'type'),
            (13, b'\xff', 'checksum'), (26, b'\x01', 'cluster low'),
            (27, b'\x01', 'cluster high'), (32, b'\x02', 'reordered'),
            (32, b'\xe5', 'deleted middle'), (43, b'\x08', 'label middle'),
            (45, b'\xff', 'inconsistent checksum'), (44, b'\x01', 'middle type'),
            (5, b'\xff\xff', 'missing terminator'), (7, b'A\x00', 'bad padding'),
            (33, b'\x00\xdc', 'lone low surrogate'),
            (33, b'\x00\xd8', 'unpaired high surrogate'),
            (33, b'/\x00', 'path separator'), (33, b'\x01\x00', 'control'),
        ]
        for offset, value, label in changes:
            with self.subTest(case=label):
                self.edit(short-64, original)
                self.edit(short-64+offset, value)
                valid = label == 'valid control'
                want = 'My dragged game' if valid else 'LONGDI~1'
                self.assertEqual(self.long_listing('/GAMES'), [(want, 'LONGDI~1')])
                self.assertEqual(self.open('/GAMES/My dragged game') >= 0, valid)
                # Close the one successful probe before the next mutation.
                self.c.asset_mount()
                self.assertEqual(self.read('/GAMES/LONGDI~1/LONGFI~1.GAM'),
                                 self.tree['GAMES']['LONGDI~1']['LONGFI~1.GAM'])
        self.assertEqual(self.c.write_count(), 0)

    def test_coherent_checksum_must_match_the_short_entry(self):
        self.mount()
        short = self.vol.entry_offset('/GAMES/LONGDI~1')
        with open(self.card, 'rb') as f:
            f.seek(short); alias = f.read(11)
        wrong = bytes([fatlfn.checksum(alias) ^ 1])
        for slot in (short-64, short-32): self.edit(slot+13, wrong)
        self.assertEqual(self.long_listing('/GAMES'), [('LONGDI~1', 'LONGDI~1')])
        self.assertEqual(self.open('/GAMES/My dragged game'), -2)
        self.assertEqual(self.read('/GAMES/LONGDI~1/LONGFI~1.GAM'), self.tree['GAMES']['LONGDI~1']['LONGFI~1.GAM'])

    def test_checksum_uses_stored_05_before_short_name_translation(self):
        self.tree = {'N0000000': b'payload'}
        self.names = {'/N0000000': 'Valid long name.gam'}
        self.mount()
        short = self.vol.entry_offset('/N0000000')
        with open(self.card, 'rb') as f:
            f.seek(short); alias = bytearray(f.read(11))
        alias[0] = 5
        self.edit(short, bytes(alias))
        for slot in (short-64, short-32): self.edit(slot+13, bytes([fatlfn.checksum(alias)]))
        self.assertEqual(self.read('/Valid long name.gam'), b'payload')

    def test_unterminated_tail_end_marker_and_unrelated_short_entry(self):
        self.tree = {'FIRST.GAM': b'first', 'SECOND.GAM': b'second'}
        self.names = {'/FIRST.GAM': 'First long file.gam'}
        self.mount()
        short = self.vol.entry_offset('/FIRST.GAM')
        self.edit(short, b'\xe5')
        self.assertEqual(self.long_listing('/'), [('SECOND.GAM', 'SECOND.GAM')])
        self.assertEqual(self.open('/First long file.gam'), -2)
        self.edit(short, b'\x00')
        self.assertEqual(self.long_listing('/'), [])

    def test_short_alias_wins_over_an_earlier_casefolded_long_name(self):
        self.tree = {'FIRST.GAM': b'long name file', 'K.GAM': b'original short file'}
        self.names = {'/FIRST.GAM': 'K.GAM'}
        self.mount()
        self.assertEqual(self.read('/K.GAM'), b'original short file')
        self.assertEqual(self.read('/k.gam'), b'original short file')
        self.assertEqual(self.read('/K.GAM'), b'long name file')
        self.assertEqual(self.read('/FIRST.GAM'), b'long name file')

    def test_cache_long_alias_is_counted_as_a_reader(self):
        self.tree = {'OS': {'CACHE': {'GAMES.IDX': b'cached'}}}
        self.names = {'/OS': 'Operating System', '/OS/CACHE': 'Cached game metadata'}
        self.mount()
        h = self.open('/Operating System/Cached game metadata/GAMES.IDX')
        self.assertGreaterEqual(h, 0)
        self.assertEqual(self.c.asset_cache_readers(), 1)
        self.c.asset_close(h)
        self.assertEqual(self.c.asset_cache_readers(), 0)

    def test_name_limits_exact_slot_lengths_and_non_normalization(self):
        names = ['X'*13, 'Y'*26, 'Z'*254, 'Q'*255, 'é'*127, 'é'*128,
                 'A'*256, 'e\u0301', 'ß.gam', '.DS_Store', '._My Game',
                 'System Volume Information']
        self.tree = {f'N{i:07}': bytes([i]) for i in range(len(names))}
        self.names = {'/'+alias: name for alias, name in zip(self.tree, names)}
        self.mount()
        expected = [(name if len(name.encode()) <= 255 and len(name) <= 255 else alias, alias)
                    for alias, name in zip(self.tree, names)]
        self.assertEqual(self.long_listing('/'), expected)
        for i, (alias, name) in enumerate(zip(self.tree, names)):
            with self.subTest(name=name):
                self.assertEqual(self.read('/'+alias), bytes([i]))
                if len(name.encode()) <= 254:
                    self.assertEqual(self.read('/'+name), bytes([i]))
        # Simple folding is not full expansion or canonical normalization.
        self.assertEqual(self.open('/é'), -2)
        self.assertEqual(self.open('/ss.gam'), -2)

    def test_invalid_utf8_paths_and_dot_components_are_rejected(self):
        self.mount()
        for part in [b'\xc0\xaf', b'\xe0\x80\xaf', b'\xed\xa0\x80',
                     b'\xf4\x90\x80\x80', b'\xf5\x80\x80\x80', b'\xe2\x82',
                     b'\x80', b'.', b'..', b'A\\B', b'A\x00B', b'A*B']:
            with self.subTest(part=part): self.assertEqual(self.open(b'/'+part), -22)

    def test_mtools_desktop_style_image(self):
        import subprocess
        import fatgen
        tool_dir = os.environ.get('MTOOLS_BIN', '')
        mcopy = shutil.which('mcopy', path=tool_dir or None)
        mkfs = shutil.which('mkfs.fat')
        if not mcopy or not mkfs: self.skipTest('mtools and mkfs.fat required for independent desktop writer')
        fatgen.build(self.card, {})
        subprocess.run([mkfs, '-F', '32', '-s', '1', '-h', '2048', '--offset', '2048', str(self.card)], check=True, capture_output=True)
        source = self.card.parent/'GAMES'
        folder = source/'My Dragged Game'
        nested = folder/'Long nested asset directory'
        nested.mkdir(parents=True)
        files = {folder/'Game Metadata.gam': b'game'*1031,
                 nested/'Café Ελληνικά asset data.bin': bytes(range(256))*7,
                 folder/'._Game Metadata.gam': b'AppleDouble',
                 folder/'.DS_Store': b'desktop metadata'}
        for path, data in files.items(): path.write_bytes(data)
        subprocess.run([mcopy, '-i', str(self.card)+'@@1048576', '-s', str(source), '::/'], check=True, capture_output=True)
        self.c.open_disk(os.fsencode(self.card)); self.addCleanup(self.c.close_disk)
        self.assertEqual(self.c.asset_mount(), 0)
        for path, data in files.items():
            card_path = '/'+path.relative_to(source.parent).as_posix()
            self.assertEqual(self.read(card_path), data)
        listing = dict(self.long_listing('/GAMES/My Dragged Game'))
        self.assertIn('Game Metadata.gam', listing)
        self.assertIn('._Game Metadata.gam', listing)  # filtering belongs to GMOS
        self.assertIn('.DS_Store', listing)
        self.assertEqual(self.read('/GAMES/My Dragged Game/'+listing['Game Metadata.gam']), files[folder/'Game Metadata.gam'])
        self.assertEqual(self.c.write_count(), 0)

    def test_unicode_case_insensitive_names_and_surrogate_pair(self):
        self.names['/GAMES/LONGDI~1'] = 'Café Ελληνικά Игра 𐐀 🎮'
        self.names['/GAMES/LONGDI~1/LONGFI~1.GAM'] = 'Écran Σς ẞ.gam'
        self.mount()
        expected = self.tree['GAMES']['LONGDI~1']['LONGFI~1.GAM']
        self.assertEqual(self.read('/games/CAFÉ ΕΛΛΗΝΙΚΆ ИГРА 𐐨 🎮/éCRAN σΣ ß.GAM'), expected)

    def test_legacy_readdir_error_precedence_is_unchanged(self):
        self.mount()
        self.assertEqual(self.c.asset_readdir(-1, None), -9)
        h = self.open('/GAMES/LONGDI~1/LONGFI~1.GAM')
        self.assertGreaterEqual(h, 0)
        self.assertEqual(self.c.asset_readdir(h, None), -20)
        self.c.asset_close(h)
        h = self.open('/GAMES'); self.assertGreaterEqual(h, 0)
        self.assertEqual(self.c.asset_readdir(h, None), -22)
        self.c.asset_close(h)

    def test_long_enumeration_preserves_legacy_short_api(self):
        self.mount()
        self.assertTrue(hasattr(self.c, 'asset_readdir_long'), 'long enumeration API missing')
        self.c.asset_readdir_long.argtypes = [C.c_int, C.POINTER(LongEntry), C.POINTER(C.c_uint32)]
        h = self.open('/GAMES')
        e, meta = LongEntry(), (C.c_uint32*3)()
        self.assertEqual(self.c.asset_readdir_long(h, C.byref(e), meta), 1)
        self.assertEqual(e.name.decode(), 'My dragged game')
        self.assertEqual(e.short_entry.name, b'LONGDI~1')
        self.assertEqual(e.short_entry.is_dir, 1)
        self.assertEqual(meta[2], self.vol.chains['/GAMES/LONGDI~1'][0])
        self.assertEqual(self.c.asset_readdir_long(h, C.byref(e), meta), 0)
        self.c.asset_close(h)
        h = self.open('/GAMES')
        old = ShortEntry()
        self.assertEqual(self.c.asset_readdir(h, C.byref(old)), 1)
        self.assertEqual(old.name, b'LONGDI~1')
        self.c.asset_close(h)

    def test_long_nested_paths_and_short_aliases_read_identical_bytes(self):
        self.mount()
        expected = self.tree['GAMES']['LONGDI~1']['LONGFI~1.GAM']
        for path in ['/GAMES/My dragged game/A long game name.gam',
                     '/games/mY DRAGGED GAME/a LONG GAME NAME.GAM',
                     '/GAMES/LONGDI~1/LONGFI~1.GAM',
                     '/GAMES/My dragged game/LONGFI~1.GAM']:
            with self.subTest(path=path): self.assertEqual(self.read(path), expected)
        self.assertEqual(self.c.write_count(), 0)


from test_m1_user import CallsBase, program, peek
import struct

class LongNameCalls(CallsBase):
    def card(self, extra=None, order=0, fb=True):
        self.disk = self.work/f'card{id(self)}.img'
        fatlfn.build(self.disk, {'OS': {'DIAG.ELF': program()}, 'LONGDI~1': {
            'A.GAM': b'asset'}}, {'/LONGDI~1': 'Long game directory with spaces'})
        self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.c.asset_mount(), 0)
        self.c.fb_setup(order, fb)
        self.assertFalse(self.c.os_start(b'/OS/DIAG.ELF'))

    def test_old_metadata_call_keeps_short_alias_with_lfn_present(self):
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        r, count = self.io(18, fd, self.BUF, 512); self.assertEqual(r, 0)
        data = peek(self.c, self.BUF, count)
        off = struct.unpack_from('<H', data)[0]
        self.assertEqual(data[off+24:off+32], b'LONGDI~1')
        self.close(fd)

    def test_metadata_long_names_and_legacy_aliases(self):
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        r, count = self.io(20, fd, self.BUF, 512); self.assertEqual(r, 0)
        data = peek(self.c, self.BUF, count)
        off = struct.unpack_from('<H', data)[0]
        length, typ, nb = struct.unpack_from('<HBB', data, off)
        self.assertEqual(data[off+24:off+24+nb], b'Long game directory with spaces')
        self.assertEqual(typ, 2)
        self.assertEqual(data[off+24+nb:off+length], bytes(length-24-nb))
        self.close(fd)
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        r, count = self.io(11, fd, self.BUF, 512); self.assertEqual(r, 0)
        data = peek(self.c, self.BUF, count)
        off = struct.unpack_from('<H', data)[0]
        self.assertEqual(data[off+8:off+16], b'LONGDI~1')
        self.close(fd)

    def test_pending_record_can_switch_long_and_short_formats(self):
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        self.assertEqual(self.io(11, fd, self.BUF, 16), (0, 16))
        self.assertEqual(self.io(20, fd, self.BUF, 32)[0], -22)
        self.assertEqual(self.io(11, fd, self.BUF, 16), (0, 16))
        self.assertEqual(peek(self.c, self.BUF+8, 8), b'LONGDI~1')
        self.assertEqual(self.io(20, fd, self.BUF, 512), (0, 0))
        self.close(fd)
        r, fd = self.open('/sd'); self.assertEqual(r, 0)
        self.assertEqual(self.io(11, fd, self.BUF, 16), (0, 16))
        self.assertEqual(self.io(11, fd, self.BUF, 8)[0], -22)
        self.assertEqual(self.io(20, fd, self.BUF, 512), (0, 56))
        self.assertEqual(peek(self.c, self.BUF+24, 31), b'Long game directory with spaces')
        self.close(fd)

    def test_syscall_opens_and_reads_long_name(self):
        r, fd = self.open('/sd/long GAME directory WITH spaces/a.gam')
        self.assertEqual(r, 0)
        self.assertEqual(self.io(2, fd, self.BUF, 20), (0, 5))
        self.assertEqual(peek(self.c, self.BUF, 5), b'asset')
        self.close(fd)
