# SPDX-License-Identifier: MIT
"""Real FAT directory path lookup: bounded I/O and invocation-local freshness.

Host fake SD only; this does not test physical-card latency or persistence.
"""
import ctypes as C
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

import fatgen
import hostlib


class Entry(C.Structure):
    _fields_ = [('name', C.c_char * 13), ('attr', C.c_uint8),
                ('is_dir', C.c_uint8), ('reserved', C.c_uint8), ('size', C.c_uint32)]


class DirectoryScan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        shim = cls.work / 'shim.c'
        shim.write_text('''#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
static FILE *f; static uint32_t reads, bad=~0u;
void open_disk(const char *p){f=fopen(p,"rb");}
void close_disk(void){fclose(f);}
uint32_t sd_sectors(void){return 264192;}
uint32_t read_count(void){return reads;}
void fail_sector(uint32_t lba){bad=lba;}
bool sd_read(uint32_t lba,void *p){reads++;return lba!=bad &&
 !fseek(f,(long)lba*512,SEEK_SET) && fread(p,1,512,f)==512;}
''')
        cls.library = cls.work / 'assets.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-DHOST_TEST',
                        '-Wall', '-Wextra', '-Werror', '-I'+str(hostlib.ROOT/'include'),
                        str(hostlib.ROOT/'core/assets.c'), str(shim), '-o', str(cls.library)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.case = pathlib.Path(tempfile.mkdtemp(dir=self.work))
        shutil.copyfile(self.library, self.case/'assets.so')
        self.c = C.CDLL(str(self.case/'assets.so'))
        self.c.open_disk.argtypes = [C.c_char_p]
        self.c.asset_open.argtypes = [C.c_char_p, C.c_uint32, C.c_uint32]
        self.c.asset_readdir.argtypes = [C.c_int, C.POINTER(Entry)]
        self.c.asset_read.argtypes = [C.c_int, C.c_void_p, C.c_uint32]
        self.disk = self.case/'card.img'
        self.tree = {'GAMES': {f'G{i:07}': {'GAME.GAM': bytes([i])*1315} for i in range(128)}}
        self.v = fatgen.build(self.disk, self.tree, spc=2, fragment=True)
        self.c.open_disk(os.fsencode(self.disk))
        self.addCleanup(self.c.close_disk)
        self.assertEqual(self.c.asset_mount(), 0)

    def opened(self, path):
        b = path.encode()
        h = self.c.asset_open(b, len(b), 0)
        self.assertGreaterEqual(h, 0, path)
        return h

    def test_path_lookup_reads_each_directory_sector_once(self):
        before = self.c.read_count()
        h = self.opened('/GAMES/G0000127/GAME.GAM')
        # Root + 9 GAMES sectors + subdirectory + FAT checks, not 130 raw-entry reads.
        self.assertLessEqual(self.c.read_count()-before, 16)
        self.assertEqual(self.c.asset_close(h), 0)

    def test_path_and_readdir_observe_edits_between_calls(self):
        h = self.opened('/GAMES')
        e = Entry()
        self.assertEqual(self.c.asset_readdir(h, C.byref(e)), 1)
        self.assertEqual(e.name, b'G0000000')
        self.c.close_disk()
        fatgen.patch(self.disk, self.v.entry_offset('/GAMES/G0000001'), b'RENAMED ')
        self.c.open_disk(os.fsencode(self.disk))
        self.assertEqual(self.c.asset_readdir(h, C.byref(e)), 1)
        self.assertEqual(e.name, b'RENAMED')
        old = b'/GAMES/G0000001'
        self.assertEqual(self.c.asset_open(old, len(old), 0), -2)
        other = self.opened('/GAMES/RENAMED')
        self.assertEqual(self.c.asset_close(other), 0)
        self.assertEqual(self.c.asset_close(h), 0)

    def test_failed_sector_is_not_cached_and_retry_keeps_entry(self):
        h = self.opened('/GAMES')
        e = Entry()
        self.c.fail_sector(self.v.lba(self.v.chains['/GAMES'][0]))
        self.assertEqual(self.c.asset_readdir(h, C.byref(e)), -5)
        self.c.fail_sector(0xffffffff)
        self.assertEqual(self.c.asset_readdir(h, C.byref(e)), 1)
        self.assertEqual(e.name, b'G0000000')
        self.assertEqual(self.c.asset_close(h), 0)

    def test_interleaved_handles_and_payload_do_not_share_directory_bytes(self):
        a, b = self.opened('/GAMES'), self.opened('/GAMES')
        f = self.opened('/GAMES/G0000127/GAME.GAM')
        e = Entry()
        data = C.create_string_buffer(512)
        for i in range(128):
            self.assertEqual(self.c.asset_readdir(a, C.byref(e)), 1)
            self.assertEqual(e.name.decode(), f'G{i:07}')
            if i == 0:
                self.assertEqual(self.c.asset_read(f, data, 512), 512)
                self.assertEqual(data.raw, bytes([127])*512)
            self.assertEqual(self.c.asset_readdir(b, C.byref(e)), 1)
            self.assertEqual(e.name.decode(), f'G{i:07}')
        self.assertEqual(self.c.asset_readdir(a, C.byref(e)), 0)
        for h in (a, b, f): self.assertEqual(self.c.asset_close(h), 0)
