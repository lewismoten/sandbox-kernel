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
        info='INFO 2 test confirmed=1 features=kernel,no-os'
        p=FakePort([info,info,'READY 1024','ACK 1024','VERIFY','OK pending rebooting'])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'runtime.ski';f.write_bytes(b)
            self.assertEqual(usb_kernel.send_kernel(p,f),0)
        self.assertEqual(p.sent[2:], [b[:1024],b[1024:]])
    def test_lost_final_never_repeats_put(self):
        b=image.pack(b'X'*4,'unknown')
        p=FakePort(['INFO 2 test confirmed=1 features=kernel,no-os','READY 1024','VERIFY',TimeoutError()])
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
