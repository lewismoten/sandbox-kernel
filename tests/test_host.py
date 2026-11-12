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
def old_sender_accepts(info):
    """The base usb_kernel.py check, verbatim: old clients need this token."""
    return 'features=kernel,no-os' in info.split()
class FsCommandTests(unittest.TestCase):
    def test_paths(self):
        self.assertEqual(usb_kernel.fs_path('/GAMES/DICE/DICE.GAM'),'GAMES/DICE/DICE.GAM');self.assertEqual(usb_kernel.fs_path('/',True),'/')
        for bad in ('games/dice','GAMES//X','../X','GAMES/A B','GAMES/TOOLONGNAME','A.B.C','','GAMES/'+'/'.join('A'*3 for _ in range(6))): 
            with self.assertRaises(ValueError,msg=bad): usb_kernel.fs_path(bad)
    def test_mv_rm_rmdir_send_one_command_and_check_ok(self):
        for cmd,reply,rc in (('MV GAMES/A GAMES/B','OK mv /GAMES/A /GAMES/B',0),('RM GAMES/A/X.GAM','ERR MOV MISSING',1),('RMDIR GAMES/A','OK rmdir /GAMES/A',0)):
            p=FakePort(['INFO 2 t',reply])
            self.assertEqual(usb_kernel.fs_command(p,cmd),rc);self.assertEqual(p.sent,[b'INFO\n',cmd.encode()+b'\n'])
    def test_ls_pages_until_next_end(self):
        p=FakePort(['INFO 2 t','ENT F 5 20 A.GAM','OK ls /GAMES next=7 skipped=0','ENT D 0 10 B','OK ls /GAMES next=end skipped=1'])
        self.assertEqual(usb_kernel.list_dir(p,'GAMES'),0);self.assertEqual(p.sent,[b'INFO\n',b'LS GAMES 0\n',b'LS GAMES 7\n'])
class SenderTests(unittest.TestCase):
    def test_old_sender_accepts_new_info_ready(self):
        import re,pathlib
        src=(pathlib.Path(__file__).parent.parent/'core'/'protocol.c').read_text()
        self.assertIn('features=kernel,no-os',src)
        for info in ('INFO 2 t updates=ready os=none features=kernel,no-os','INFO 2 t updates=ready os=running features=kernel,no-os'):
            self.assertTrue(old_sender_accepts(info));usb_kernel.check_updates(usb_kernel.info_fields(info))
    def _bad(self,info):
        with self.assertRaises(ValueError): usb_kernel.check_updates(usb_kernel.info_fields(info))
    def test_conflicting_or_odd_update_fields_fail_closed(self):
        for t in ('updates=ready updates=locked features=locked','updates=locked updates=ready features=locked','updates=ready updates= features=locked',
                  'updates= features=kernel,no-os','updates=maybe','updates=READY','updates=busy','updates=ready features=locked','updates=ready updates=ready',
                  'xupdates=ready','note=updates=ready','features=kernel','features=locked','features=kernel,no-os features=kernel,no-os',''):
            self._bad('INFO 2 t '+t)
    def test_busy_fails_closed_with_a_clear_message_and_sends_no_put2(self):
        info='INFO 2 t confirmed=1 pending=2 updates=busy os=none features=kernel,no-os'
        p=FakePort([info,info])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'runtime.ski';f.write_bytes(image.pack(b'X'*64,'sender'))
            with self.assertRaisesRegex(ValueError,'pending update'): usb_kernel.send_kernel(p,f)
        self.assertFalse(any(s.startswith(b'PUT2') for s in p.sent))
    def test_pending_must_be_a_single_zero(self):
        for t in ('pending=2','pending=1','pending=0 pending=0','pending=','pending=banana','pending=-1','pending=0x0','pending=\u0661'):
            self._bad('INFO 2 t confirmed=1 '+t+' updates=ready os=none features=kernel,no-os')
        for t in ('pending=0','pending=00',''):
            usb_kernel.check_updates(usb_kernel.info_fields('INFO 2 t confirmed=1 '+t+' updates=ready os=none features=kernel,no-os'))
    def test_unrelated_fields_do_not_block_legacy(self):
        for t in ('xupdates=locked features=kernel,no-os','note=updates=locked features=kernel,no-os'):
            usb_kernel.check_updates(usb_kernel.info_fields('INFO 2 t '+t))
    def test_first_info_retries(self):
        p=FakePort([TimeoutError(),TimeoutError(),'INFO 2 test'])
        self.assertEqual(usb_kernel.handshake(p),'INFO 2 test')
        self.assertEqual(p.sent,[b'INFO\n']*3)
    def test_exact_multiple_final_uses_verify(self):
        b=image.pack(b'X'*1792,'sender') # 2048-byte transport, two full blocks
        info='INFO 2 test confirmed=1 updates=ready os=none features=kernel,no-os'
        p=FakePort([info,info,'READY 1024','ACK 1024','VERIFY','OK pending rebooting'])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'runtime.ski';f.write_bytes(b)
            self.assertEqual(usb_kernel.send_kernel(p,f),0)
        self.assertEqual(p.sent[2:], [b[:1024],b[1024:]])
    def test_lost_final_never_repeats_put(self):
        b=image.pack(b'X'*4,'unknown')
        p=FakePort(['INFO 2 test confirmed=1 updates=ready os=none features=kernel,no-os','READY 1024','VERIFY',TimeoutError()])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'runtime.ski';f.write_bytes(b)
            self.assertEqual(usb_kernel.send_kernel(p,f),2)
        self.assertEqual(sum(s.startswith(b'PUT2') for s in p.sent),1)
    def test_install_host_command_and_ack(self):
        b=image.pack(b'H'*4,'host',role='host')
        info='INFO 2 test confirmed=1 pending=0 host_companion=0 updates=ready'
        p=FakePort([info,'READY 1024','VERIFY','OK host companion installed'])
        with tempfile.TemporaryDirectory() as d:
            f=pathlib.Path(d)/'host.ski';f.write_bytes(b)
            self.assertEqual(usb_kernel.send_kernel(p,f,companion=True),0)
        self.assertTrue(p.sent[1].startswith(b'PUT2 host-companion zero-w-armv6 '))
        self.assertEqual(p.sent[2],b)

    def test_install_host_refuses_legacy_receiver_and_device_payload(self):
        for role,extra,why in [('host','','Update the device kernel first'),('device',' host_companion=0','USB_ROLE=host')]:
            p=FakePort(['INFO 2 test pending=0 updates=ready'+extra])
            with tempfile.TemporaryDirectory() as d:
                f=pathlib.Path(d)/'runtime.ski';f.write_bytes(image.pack(b'X'*4,'role',role=role))
                with self.assertRaisesRegex(ValueError,why):usb_kernel.send_kernel(p,f,companion=True)
            self.assertEqual(p.sent,[b'INFO\n'])

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
