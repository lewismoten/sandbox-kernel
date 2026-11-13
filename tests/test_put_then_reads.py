# SPDX-License-Identifier: MIT
"""After a put (and after an rm), reads and further puts into a new folder keep working."""
import unittest
from test_m1_user import InstallBase, text

class PutThenReads(InstallBase):
    def put(self, p, d):
        self.assertEqual(self.c.fatput(p.encode(), d, len(d)), 0, text(self.c, 'fatput_fail'))

    def test_second_file_in_new_game_folder_then_reads(self):
        a, b = b'credits' * 3000, b'second' * 5000
        self.put('GAMES/STARS/CREDITS.GIF', a)
        self.put('GAMES/STARS/STARS.GAM', b)
        self.assertEqual(self.read('/GAMES/STARS/CREDITS.GIF'), a)
        self.assertEqual(self.read('/GAMES/STARS/STARS.GAM'), b)
        for i in range(36): self.put('GAMES/STARS/F%02d.DAT' % i, bytes([i]) * 700)
        self.assertEqual(self.read('/GAMES/STARS/F35.DAT'), bytes([35]) * 700)
        self.assertEqual(self.read('/GAMES/STARS/CREDITS.GIF'), a)

    def test_put_rm_then_folder_puts(self):
        self.put('GAMES/OLD/LIB.JSL', b'lib' * 2000)
        self.assertEqual(self.c.fatfs_rm(b'GAMES/OLD/LIB.JSL'), 0, text(self.c, 'fatput_fail'))
        self.put('GAMES/STARS/CREDITS.GIF', b'x' * 9000)
        self.put('GAMES/STARS/B.GIF', b'y' * 9000)
        self.assertEqual(self.read('/GAMES/STARS/B.GIF'), b'y' * 9000)
