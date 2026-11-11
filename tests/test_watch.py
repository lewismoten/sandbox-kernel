# SPDX-License-Identifier: MIT
"""Host watch CLI exercised through the real capture reader with a fake serial port."""
import datetime
import pathlib
import sys
import tempfile
import unittest
import zlib
from unittest import mock
from urllib.parse import unquote
from html.parser import HTMLParser

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import usb_kernel as usb


class Port:
    def __init__(self,frames):
        self.frames={k:iter(v) for k,v in frames.items()};self.sent=[];self.closed=False
    def send(self,command):
        self.sent.append(command)
        if command==b'INFO\n': self.header='INFO 2 fake';return
        fields=command.decode().split();kind='EINK' if fields[0]=='EINKSHOT' else 'FB'
        offset=int(fields[-1])
        if offset==0: self.data=next(self.frames[kind])
        block=self.data[offset:offset+256]
        geometry='122 250 16 1 1' if kind=='EINK' else '1 1 4 32 0'
        self.header=f'SHOT {kind} 1 {geometry} {len(self.data)} {offset} {len(block)} {zlib.crc32(block)}'
        self.block=block
    def reply(self,timeout): return self.header
    def line(self,timeout): return self.header
    def exact(self,n):
        assert n==len(self.block)
        return self.block
    def close(self): self.closed=True


class Images(HTMLParser):
    def __init__(self,text):
        super().__init__();self.sources=[];self.feed(text)
    def handle_starttag(self,tag,attrs):
        if tag=='img': self.sources.append(unquote(dict(attrs)['src']))


class Watch(unittest.TestCase):
    def run_cli(self,port,folder,*args):
        argv=['usb_kernel.py','fake','watch','--out',str(folder),*args]
        with mock.patch.object(sys,'argv',argv),mock.patch.object(usb,'Serial',return_value=port):
            return usb.main()
    def test_default_eink_deduplicates_consecutive_frames_and_keeps_returning_frame(self):
        a=bytes(4000);b=b'\xff'*4000
        port=Port({'EINK':[a,a,b,b,a]})
        with tempfile.TemporaryDirectory() as d,mock.patch.object(usb.time,'sleep'):
            folder=pathlib.Path(d)
            self.assertEqual(self.run_cli(port,folder,'--count','5'),0)
            files=sorted(folder.glob('*.png'));self.assertEqual(len(files),3)
            self.assertEqual(files[0].read_bytes(),files[2].read_bytes())
            self.assertNotEqual(files[0].read_bytes(),files[1].read_bytes())
            for path in files:
                stamp=path.name.removesuffix('-eink.png')
                self.assertEqual(datetime.datetime.fromisoformat(stamp.replace('Z','+00:00')).utcoffset().total_seconds(),0)
            self.assertEqual(Images((folder/'index.html').read_text()).sources,[p.name for p in files])
        self.assertTrue(port.closed)
        self.assertEqual(sum(c==b'EINKSHOT 0 0\n' for c in port.sent),5)
    def test_both_displays_have_independent_hashes_and_hdmi_scale(self):
        port=Port({'EINK':[bytes(4000)]*3,'FB':[bytes(4),bytes(4),b'\xff'*4]})
        with tempfile.TemporaryDirectory() as d,mock.patch.object(usb.time,'sleep'):
            self.assertEqual(self.run_cli(port,d,'--eink','--hdmi','--scale','3','--count','3'),0)
            folder=pathlib.Path(d)
            self.assertEqual(len(list(folder.glob('*-eink.png'))),1)
            self.assertEqual(len(list(folder.glob('*-hdmi.png'))),2)
        self.assertEqual(sum(c==b'FBSHOT 3 0 0\n' for c in port.sent),3)
    def test_hdmi_only_and_interval_without_final_sleep(self):
        port=Port({'FB':[bytes(4)]*2})
        with tempfile.TemporaryDirectory() as d,mock.patch.object(usb.time,'sleep') as sleep,mock.patch.object(usb.time,'monotonic',return_value=10):
            self.assertEqual(self.run_cli(port,d,'--hdmi','--every','2.5','--count','2'),0)
            self.assertFalse(any(c.startswith(b'EINKSHOT') for c in port.sent))
            self.assertEqual([c for c in sleep.call_args_list if c==mock.call(2.5)],[mock.call(2.5)])
    def test_ctrl_c_during_wait_or_capture_closes_port_and_preserves_index(self):
        for where in ('wait','capture'):
            port=Port({'FB':[bytes(4)]})
            def sleep(seconds):
                if seconds!=0.002 and where=='wait': raise KeyboardInterrupt
            original=port.send
            def send(command):
                if where=='capture' and command==b'FBSHOT 1 0 0\n' and port.sent.count(command): raise KeyboardInterrupt
                original(command)
            with tempfile.TemporaryDirectory() as d,mock.patch.object(usb.time,'sleep',side_effect=sleep),mock.patch.object(port,'send',side_effect=send):
                self.assertEqual(self.run_cli(port,d,'--hdmi'),0)
                folder=pathlib.Path(d);files=list(folder.glob('*.png'))
                self.assertEqual(len(files),1)
                self.assertEqual(Images((folder/'index.html').read_text()).sources,[files[0].name])
            self.assertTrue(port.closed)
    def test_existing_png_is_preserved_and_html_names_are_escaped(self):
        with tempfile.TemporaryDirectory() as d,mock.patch.object(usb.time,'sleep'):
            folder=pathlib.Path(d);old=folder/'old & "<frame>.png';old.write_bytes(b'old')
            port=Port({'FB':[bytes(4)]})
            self.run_cli(port,folder,'--hdmi','--count','1')
            page=(folder/'index.html').read_text()
            self.assertIn(old.name,Images(page).sources);self.assertIn('&lt;frame&gt;',page)
            self.assertEqual(old.read_bytes(),b'old')
    def test_repeated_timestamp_does_not_overwrite_an_existing_frame(self):
        stamp=datetime.datetime(2026,11,11,tzinfo=datetime.timezone.utc)
        port=Port({'FB':[bytes(4),b'\xff'*4]})
        with tempfile.TemporaryDirectory() as d,mock.patch.object(usb.time,'sleep'),mock.patch.object(usb.datetime,'datetime') as clock:
            clock.now.return_value=stamp
            folder=pathlib.Path(d);old=folder/'2026-11-11T00:00:00.000000Z-hdmi.png';old.write_bytes(b'old')
            self.assertEqual(self.run_cli(port,folder,'--hdmi','--count','2'),0)
            self.assertEqual(old.read_bytes(),b'old')
            self.assertEqual(len(list(folder.glob('*.png'))),3)
            self.assertEqual(len(Images((folder/'index.html').read_text()).sources),3)
    def test_bad_options_are_rejected_before_opening_port(self):
        for args in (['--count','0'],['--count','-1'],['--every','0'],['--every','-1'],['--every','nan'],['--every','inf'],['--scale','17'],['--scale','2'],[]):
            argv=['usb_kernel.py','fake','watch',*(['--out','unused'] if args else []),*args]
            with self.subTest(args=args),mock.patch.object(sys,'argv',argv),mock.patch.object(usb,'Serial') as serial,mock.patch.object(sys,'stderr'):
                with self.assertRaises(SystemExit) as exc: usb.main()
                self.assertEqual(exc.exception.code,2);serial.assert_not_called()
    def test_capture_failure_leaves_no_png_and_closes_port(self):
        port=Port({'FB':[bytes(4)]})
        with tempfile.TemporaryDirectory() as d,mock.patch.object(port,'exact',return_value=b'bad!'):
            with self.assertRaisesRegex(ValueError,'CRC'): self.run_cli(port,d,'--hdmi','--count','1')
            folder=pathlib.Path(d)
            self.assertEqual(list(folder.glob('*.png')),[])
            self.assertEqual(Images((folder/'index.html').read_text()).sources,[])
        self.assertTrue(port.closed)


if __name__=='__main__': unittest.main()
