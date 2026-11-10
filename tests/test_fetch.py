"""TinyUSB fetch: a version switch leaves no stale files, and the Makefile stamp is version-specific."""
import importlib.util, io, json, re, subprocess, sys, tarfile, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fetch', ROOT / 'scripts/fetch.py')
fetch = importlib.util.module_from_spec(spec); spec.loader.exec_module(fetch)

def archive(files):
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode='w:gz') as t:
        for name, data in files.items():
            i = tarfile.TarInfo('tinyusb-x/' + name); i.size = len(data); t.addfile(i, io.BytesIO(data))
    return b.getvalue()

class FetchTests(unittest.TestCase):
    def test_version_switch_leaves_no_stale_files(self):
        with tempfile.TemporaryDirectory() as d:
            dest = Path(d) / 'tinyusb'
            old = archive({'LICENSE': b'l', 'src/device/usbd.c': b'old', 'src/device/usbd_control.c': b'ctl',
                           'src/class/cdc/cdc_rndis_host.c': b'r'})
            fetch.install_tinyusb(old, dest, '0.18.0', 'TinyUSB 0.18.0')
            self.assertTrue((dest / 'src/device/usbd_control.c').exists())
            new = archive({'LICENSE': b'l', 'src/device/usbd.c': b'new'})
            fetch.install_tinyusb(new, dest, '0.21.0', 'TinyUSB 0.21.0')
            self.assertFalse((dest / 'src/device/usbd_control.c').exists())
            self.assertFalse((dest / 'src/class/cdc/cdc_rndis_host.c').exists())
            self.assertFalse((dest / '.fetched-0.18.0').exists())
            self.assertTrue((dest / '.fetched-0.21.0').exists())
            files = json.loads((dest / 'SHA256.json').read_text())['files']
            self.assertEqual(sorted(files), ['LICENSE', 'src/device/usbd.c'])
            self.assertEqual((dest / 'src/device/usbd.c').read_bytes(), b'new')
            self.assertFalse(Path(str(dest) + '.new').exists())

    def test_makefile_stamp_names_the_pinned_version(self):
        mk = (ROOT / 'Makefile').read_text()
        self.assertEqual(re.search(r'^TUSB_VERSION := (\S+)', mk, re.M).group(1), fetch.TINYUSB_VERSION)
        self.assertIn('.fetched-$(TUSB_VERSION)', mk)

    def test_old_unversioned_stamp_does_not_satisfy_deps(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'third_party/tinyusb').mkdir(parents=True)
            (Path(d) / 'third_party/tinyusb/.fetched').write_text('TinyUSB 0.18.0\n')
            (Path(d) / 'scripts').mkdir(); (Path(d) / 'scripts/fetch.py').write_text('')
            (Path(d) / 'Makefile').write_text((ROOT / 'Makefile').read_text())
            r = subprocess.run(['make', '-n', 'deps', 'PYTHON=echo'], cwd=d, capture_output=True, text=True)
            self.assertIn('fetch.py tinyusb', r.stdout)

if __name__ == '__main__':
    unittest.main(verbosity=2)
