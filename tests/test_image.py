# SPDX-License-Identifier: MIT
"""Boot image metadata must be internally consistent on every checker platform."""
import json
import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'scripts'))
import image


class FatImage(unittest.TestCase):
    def test_volume_label_matches_primary_and_backup_boot(self):
        with tempfile.TemporaryDirectory() as work:
            work = pathlib.Path(work)
            folder = work / 'boot'
            folder.mkdir()
            for name in ('BOOTCT0.BIN', 'KERNA.BIN', 'BOOTCT1.BIN', 'KERNB.BIN'):
                (folder / name).write_bytes(name.encode())
            disk = work / 'boot.img'
            image.fat_image(folder, disk)
            with disk.open('rb') as src:
                src.seek(2048 * 512)
                boot = src.read(512)
                src.seek((2048 + 6) * 512)
                self.assertEqual(src.read(512), boot)
                data_lba = 2048 + 32 + 2 * 2048
                src.seek(data_lba * 512)
                root = src.read(1024)
                labels = [root[i:i+32] for i in range(0, 1024, 32)
                          if root[i] not in (0, 0xe5) and root[i+11] == 8]
                self.assertEqual(len(labels), 1, 'root must have one volume label')
                self.assertEqual(labels[0][:11], boot[71:82])
                self.assertEqual(labels[0][20:22], bytes(2))
                self.assertEqual(labels[0][26:32], bytes(6))
                layout = json.loads(disk.with_suffix('.layout.json').read_text())['files']
                for name, entry in layout.items():
                    src.seek(entry['first_lba'] * 512)
                    self.assertEqual(src.read(entry['bytes']), (folder / name).read_bytes())
                    self.assertIn(image.short_name(name), root)
