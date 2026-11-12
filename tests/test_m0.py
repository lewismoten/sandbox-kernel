# SPDX-License-Identifier: MIT
import ctypes as C
import hashlib, importlib.util, os, pathlib, random, shutil, struct, subprocess, sys, tempfile, unittest, zlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'));sys.path.insert(0,str(ROOT/'tests'))
import image

class CoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.work=pathlib.Path(cls.temp.name)
        cls.lib=cls.work/'core.dylib'
        import hostlib;hostlib.build(cls.lib)
        cls.folder=cls.work/'boot';cls.folder.mkdir()
        cls.known=image.pack(bytes(range(256))*4,'known')
        (cls.folder/'KERNA.BIN').write_bytes(cls.known.ljust(image.MAX_IMAGE,b'\0'))
        (cls.folder/'KERNB.BIN').write_bytes(bytes(image.MAX_IMAGE))
        record=image.control_record(cls.known)
        for n in (0,1): (cls.folder/f'BOOTCT{n}.BIN').write_bytes(record+bytes(3584))
        cls.disk=cls.work/'base.img';image.fat_image(cls.folder,cls.disk)
        import json
        cls.layout=json.loads(cls.disk.with_suffix('.layout.json').read_text())['files']
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();p=pathlib.Path(self.tmp.name)
        shutil.copyfile(self.lib,p/'test.dylib');self.c=C.CDLL(str(p/'test.dylib'))
        # Sparse media clone avoids disk-full pressure from multiple cases.
        self.media=p/'card.img'
        with self.disk.open('rb') as src,self.media.open('wb') as dst:
            dst.truncate(self.disk.stat().st_size)
            for lba in range(0,22000,1024):
                src.seek(lba*512);dst.seek(lba*512);dst.write(src.read(1024*512))
        self.c.open_disk.argtypes=[C.c_char_p];self.c.open_disk(os.fsencode(self.media))
        self.c.initialize.restype=C.c_bool;self.assertTrue(self.c.initialize())
        self.c.take_output.restype=C.c_char_p
        self.c.image_valid.argtypes=[C.c_void_p,C.c_uint32];self.c.image_valid.restype=C.c_bool
        self.before=self.confirmed_hash()
    def tearDown(self):
        self.assertEqual(self.confirmed_hash(),self.before,'confirmed slot was modified')
        self.c.close_disk();self.tmp.cleanup()
    def confirmed_hash(self):
        with self.media.open('rb') as f:
            f.seek(self.layout['KERNA.BIN']['first_lba']*512);return hashlib.sha256(f.read(image.MAX_IMAGE)).digest()
    def cmd(self,b):
        if isinstance(b,str): b=b.encode()
        self.c.feed(b,len(b));self.c.protocol_task();return self.c.take_output().decode()
    def begin(self,payload,crc=None):
        self.assertTrue(self.cmd('INFO\n').startswith('INFO 2 '))
        return self.cmd(f'PUT2 kernel zero-w-armv6 {len(payload)} {(zlib.crc32(payload) if crc is None else crc):08x}\n')
    def transfer(self,payload):
        for at in range(0,len(payload),1024):
            response=self.cmd(payload[at:at+1024])
            self.assertEqual(response,'VERIFY\n' if at+1024>=len(payload) else f'ACK {at+1024}\n')
    def install(self):
        reply=''
        for _ in range(9000):
            self.c.protocol_task();reply+=self.c.take_output().decode()
            if 'ERR ' in reply or 'OK ' in reply: break
        return reply
    def test_hash_crc_and_manifest(self):
        for n in [0,1,55,56,63,64,65,1000,2097152]:
            b=random.Random(n).randbytes(n);out=C.create_string_buffer(32);self.c.sha256(b,len(b),out)
            self.assertEqual(out.raw,hashlib.sha256(b).digest())
            self.c.crc32.restype=C.c_uint32;self.assertEqual(self.c.crc32(b,len(b)),zlib.crc32(b))
        self.assertTrue(self.c.image_valid(self.known,len(self.known)))
        bad=bytearray(self.known);bad[256]^=1;self.assertFalse(self.c.image_valid(bytes(bad),len(bad)))
    def test_uptime_and_division(self):
        for sec,wanted in [(42,'42s'),(2940,'49m'),(3900,'1h 5m'),(187200,'2d 4h'),(4320000,'7w 1d'),(58752000,'1y 45w')]:
            b=C.create_string_buffer(32);self.c.uptime(b,sec);self.assertEqual(b.value.decode(),wanted)
        getattr(self.c,'__aeabi_uidivmod').argtypes=[C.c_uint32,C.c_uint32]
        getattr(self.c,'__aeabi_uidivmod').restype=C.c_uint64
        for _ in range(1000):
            n=random.randrange(2**32);d=random.randrange(1,2**32);r=getattr(self.c,'__aeabi_uidivmod')(n,d)
            self.assertEqual((r&0xffffffff,r>>32),divmod(n,d))
    def test_boundaries(self):
        self.assertEqual(self.cmd('x'*300+'\n'),'ERR command-too-long\n')
        for s in ['PUT2 kernel wrong 64 00000000\n','PUT2 os zero-w-armv6 64 00000000\n','PUT2 kernel zero-w-armv6 42949672960 00000000\n','PUT2 kernel zero-w-armv6 2097153 00000000\n']:
            self.assertTrue(self.cmd(s).startswith('ERR '))
        self.assertEqual(self.c.write_count(),0)
    def test_crc_rejection_and_timeout(self):
        self.assertEqual(self.begin(self.known,0),'READY 1024\n');self.transfer(self.known)
        self.assertIn('ERR crc',self.install());self.assertEqual(self.c.write_count(),0)
        self.assertEqual(self.begin(self.known),'READY 1024\n');self.cmd(self.known[:20]);self.c.advance(15000001)
        self.assertIn('ERR timeout',self.cmd(b''))
    def test_trailing_input(self):
        self.begin(self.known);self.transfer(self.known);self.c.feed(b'X',1)
        self.assertIn('ERR trailing-input',self.install());self.assertEqual(self.c.write_count(),0)
    def test_success_and_consumption(self):
        candidate=image.pack(b'B'*768,'next') # exact 1024-byte wire image
        self.assertEqual(self.begin(candidate),'READY 1024\n');self.transfer(candidate)
        self.assertEqual(self.install(),'OK pending rebooting\n')
        ctl=(C.c_uint32*128).in_dll(self.c,'control');self.assertEqual((ctl[3],ctl[4],ctl[5]),(0,1,0))
        ctl[5]=1;self.c.control_commit.argtypes=[C.POINTER(C.c_uint32)];self.c.control_commit.restype=C.c_bool
        self.assertTrue(self.c.control_commit(ctl));self.assertEqual(ctl[5],1)
    def install_companion(self, candidate):
        self.cmd('INFO\n')
        self.assertEqual(self.cmd(f'PUT2 host-companion zero-w-armv6 {len(candidate)} {zlib.crc32(candidate):08x}\n'),'READY 1024\n')
        self.transfer(candidate)
        return self.install()

    def test_host_companion_survives_mount_and_keeps_confirmed(self):
        candidate=image.pack(b'H'*768,'host',role='host')
        self.assertEqual(self.install_companion(candidate),'OK host companion installed\n')
        ctl=(C.c_uint32*128).in_dll(self.c,'control')
        self.assertEqual((ctl[3],ctl[4],ctl[5],ctl[6],ctl[7],ctl[9]),(0,image.NO_SLOT,0,1,0,2))
        self.assertEqual(bytes(ctl)[128:160],hashlib.sha256(candidate).digest())
        for _ in range(3):
            self.assertTrue(self.c.initialize())
            self.assertEqual((ctl[3],ctl[4],ctl[9]),(0,image.NO_SLOT,2))
            dest=C.create_string_buffer(image.MAX_IMAGE);n=C.c_uint32()
            self.c.slot_load.restype=C.c_bool
            self.assertTrue(self.c.slot_load(1,dest,C.byref(n)))
            self.assertEqual(dest.raw[:n.value],candidate)
        self.assertIn('host_companion=2',self.cmd('INFO\n'))
        self.assertIn('updates=ready',self.cmd('INFO\n'))
        # A damaged companion cannot pass the same loader used for handoff;
        # mounting and verifying the confirmed device still work.
        with self.media.open('r+b') as f:
            f.seek(self.layout['KERNB.BIN']['first_lba']*512+256);f.write(b'!')
        self.assertTrue(self.c.initialize())
        self.assertFalse(self.c.slot_load(1,dest,C.byref(n)))

    def test_companion_rejects_device_image_before_any_write(self):
        self.assertIn('ERR host-role-required',self.install_companion(self.known))
        self.assertEqual(self.c.write_count(),0)

    def test_normal_update_revokes_companion_before_slot_write(self):
        candidate=image.pack(b'H'*768,'host',role='host')
        self.assertEqual(self.install_companion(candidate),'OK host companion installed\n')
        self.assertTrue(self.c.initialize())
        self.begin(self.known);self.transfer(self.known)
        self.c.inject_failure(self.c.write_count()+1) # revocation succeeds, slot write fails
        self.assertIn('ERR write',self.install())
        self.assertTrue(self.c.store_init())
        ctl=(C.c_uint32*128).in_dll(self.c,'control')
        self.assertEqual((ctl[3],ctl[4],ctl[9]),(0,image.NO_SLOT,0))
        self.assertEqual(bytes(ctl)[128:160],bytes(32))

    def test_companion_cannot_be_factory_confirmed(self):
        with self.assertRaises(ValueError):image.control_record(image.pack(b'H'*4,'host',role='host'))

    def busy(self,n):C.c_uint32.in_dll(self.c,'card_busy').value=n
    def test_install_waits_for_an_os_file_operation_in_flight(self):
        self.begin(self.known);self.transfer(self.known);self.busy(1)
        for _ in range(50):self.c.protocol_task()
        self.assertEqual(self.c.write_count(),0)   # nothing written while the OS owns the card
        self.assertEqual(self.c.take_output().decode(),'')
        self.busy(0)
        self.assertIn('OK pending',self.install())
        ctl=(C.c_uint32*128).in_dll(self.c,'control');self.assertNotEqual(ctl[4],image.NO_SLOT)
    def test_install_busy_card_times_out_without_locking_updates(self):
        self.begin(self.known);self.transfer(self.known);self.busy(1)
        for _ in range(5):self.c.protocol_task()
        self.c.advance(6000000)
        self.assertIn('ERR card-busy',self.install());self.busy(0)
        self.assertEqual(self.c.write_count(),0)
        self.assertIn('updates=ready',self.cmd('INFO\n'))
    def test_transient_slot_write_error_is_retried_once(self):
        self.begin(self.known);self.transfer(self.known);self.c.fail_write_once(2)
        self.assertIn('OK pending',self.install())
        ctl=(C.c_uint32*128).in_dll(self.c,'control');self.assertNotEqual(ctl[4],image.NO_SLOT)
    def test_write_failure_never_pending(self):
        self.begin(self.known);self.transfer(self.known);self.c.inject_failure(1)
        self.assertIn('ERR write',self.install());ctl=(C.c_uint32*128).in_dll(self.c,'control');self.assertEqual(ctl[4],image.NO_SLOT)
    def test_bad_manifest_never_writes(self):
        for offset in [8,12,20,24,32,65,96,256]:
            b=bytearray(self.known);b[offset]^=255
            self.begin(bytes(b));self.transfer(bytes(b))
            self.assertIn('ERR manifest',self.install())
        self.assertEqual(self.c.write_count(),0)
    def test_control_copy_failure_and_recovery(self):
        self.begin(self.known);self.transfer(self.known)
        self.assertEqual(self.install(),'OK pending rebooting\n')
        # Sequence 2 lives in copy 1. A torn/corrupt newer record leaves copy 0.
        lba=self.layout['BOOTCT1.BIN']['first_lba']
        with self.media.open('r+b') as f: f.seek(lba*512);f.write(b'BAD!')
        self.assertTrue(self.c.initialize())
        ctl=(C.c_uint32*128).in_dll(self.c,'control')
        self.assertEqual((ctl[2],ctl[3],ctl[4]),(1,0,image.NO_SLOT))
    def test_both_control_copies_invalid_lock_updates(self):
        with self.media.open('r+b') as f:
            for name in ['BOOTCT0.BIN','BOOTCT1.BIN']:
                f.seek(self.layout[name]['first_lba']*512);f.write(b'BAD!')
        self.assertFalse(self.c.initialize())
        self.assertEqual(self.begin(self.known),'ERR not-ready\n')
        self.assertEqual(self.c.write_count(),0)
    def test_fat_overlap_rejected(self):
        # Make B's directory cluster point to A; no write can be admitted.
        data_lba=2048+32+4096
        with self.media.open('r+b') as f:
            f.seek(data_lba*512);root=bytearray(f.read(1024))
            a=root.find(b'KERNA   BIN');b=root.find(b'KERNB   BIN')
            root[b+20:b+22]=root[a+20:a+22];root[b+26:b+28]=root[a+26:a+28]
            f.seek(data_lba*512);f.write(root)
        self.assertFalse(self.c.initialize());self.assertEqual(self.c.write_count(),0)
    def test_commit_failure_has_no_ram_pending(self):
        b=image.pack(b'C'*768,'next');self.begin(b);self.transfer(b)
        # Two slot sectors succeed; metadata write fails.
        self.c.inject_failure(2)
        self.assertIn('ERR commit-uncertain',self.install())
        ctl=(C.c_uint32*128).in_dll(self.c,'control');self.assertEqual(ctl[4],image.NO_SLOT)
    def test_short_final_and_minimum_payload(self):
        b=image.pack(b'Q'*769,'short-final');self.begin(b);self.transfer(b)
        self.assertEqual(self.install(),'OK pending rebooting\n')

    def test_maximum_transfer(self):
        b=image.pack(b'Z'*(image.MAX_IMAGE-256),'maximum');self.begin(b);self.transfer(b)
        self.assertEqual(self.install(),'OK pending rebooting\n')

if __name__=='__main__': unittest.main()
