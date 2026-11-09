# SPDX-License-Identifier: MIT
import pathlib, sys, tempfile, unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import usb_kernel, image
class FakePort:
    def __init__(self,replies): self.replies=iter(replies);self.sent=[]
    def send(self,data): self.sent.append(data)
    def reply(self,timeout):
        r=next(self.replies)
        if isinstance(r,Exception): raise r
        return r
class SenderTests(unittest.TestCase):
    def test_first_info_retries(self):
        p=FakePort([TimeoutError(),TimeoutError(),'INFO 2 test'])
        self.assertEqual(usb_kernel.handshake(p),'INFO 2 test')
        self.assertEqual(p.sent,[b'INFO\n']*3)
    def test_exact_multiple_final_uses_verify(self):
        b=image.pack(b'X'*1792,'sender') # 2048-byte transport, two full blocks
        info='INFO 2 test confirmed=1 updates=ready os=none features=kernel-update'
        p=FakePort([info,info,'READY 1024','ACK 1024','VERIFY','OK pending rebooting'])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'runtime.ski';f.write_bytes(b)
            self.assertEqual(usb_kernel.send_kernel(p,f),0)
        self.assertEqual(p.sent[2:], [b[:1024],b[1024:]])
    def test_lost_final_never_repeats_put(self):
        b=image.pack(b'X'*4,'unknown')
        p=FakePort(['INFO 2 test confirmed=1 updates=ready os=none features=kernel-update','READY 1024','VERIFY',TimeoutError()])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'runtime.ski';f.write_bytes(b)
            self.assertEqual(usb_kernel.send_kernel(p,f),2)
        self.assertEqual(sum(s.startswith(b'PUT2') for s in p.sent),1)
    def test_put_file_blocks_and_hash(self):
        import hashlib, zlib
        data=b'E'*1500;info='INFO 2 test confirmed=1'
        p=FakePort([info,'READY 1024','ACK 1024','VERIFY','OK file /OS/DIAG.ELF 1500'])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'diag.elf';f.write_bytes(data)
            self.assertEqual(usb_kernel.send_file(p,f,'DIAG.ELF'),0)
            self.assertEqual(p.sent[1],f'PUTF DIAG.ELF 1500 {zlib.crc32(data):08x} {hashlib.sha256(data).hexdigest()}\n'.encode())
            self.assertEqual(p.sent[2:],[data[:1024],data[1024:]])
            for bad in ('diag.elf','../X','TOOLONGNAME.ELF'):
                with self.assertRaises(ValueError): usb_kernel.send_file(FakePort([]),f,bad)
    def test_put_game_path(self):
        info='INFO 2 test confirmed=1'
        p=FakePort([info,'READY 1024','VERIFY','OK file /GAMES/DICE/DICE.GAM 3'])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'dice.gam';f.write_bytes(b'GM!')
            self.assertEqual(usb_kernel.send_file(p,f,'GAMES/DICE/DICE.GAM'),0)
            self.assertTrue(p.sent[1].startswith(b'PUTF GAMES/DICE/DICE.GAM 3 '))
            for bad in ('GAMES/DICE.X/A.GAM','OTHER/DICE/A.GAM','GAMES/A.GAM','games/dice/a.gam','GAMES/DICE/A/B.GAM','GAMES/A.G00','POCKET/LIBRARY.JSL','OTHER/A.GMP'):
                with self.assertRaises(ValueError): usb_kernel.send_file(FakePort([]),f,bad)
    def test_put_packed_game(self):
        info='INFO 2 test confirmed=1'
        for name in ('GAMES/DICE.GMP','POCKET/DICE.GMP','GAMES/DICE.G01','GAMES/LIBRARY.JSL'):
            p=FakePort([info,'READY 1024','VERIFY',f'OK file /{name} 3'])
            with tempfile.TemporaryDirectory() as d:
                f=pathlib.Path(d)/'x';f.write_bytes(b'GMS')
                self.assertEqual(usb_kernel.send_file(p,f,name),0,name)
                self.assertTrue(p.sent[1].startswith(f'PUTF {name} 3 '.encode()))
    def test_keys_press_and_release(self):
        p=FakePort(['INFO 2 test','OK key 108 down','OK key 108 up','OK key 28 down',TimeoutError()])
        self.assertEqual(usb_kernel.send_keys(p,['down','enter:down'],0,0),0)
        self.assertEqual(p.sent[1:],[b'KEY 108 down\n',b'KEY 108 up\n',b'KEY 28 down\n'])
        p=FakePort(['INFO 2 test','ERR input no-device'])
        self.assertEqual(usb_kernel.send_keys(p,['q'],0,0),1)
