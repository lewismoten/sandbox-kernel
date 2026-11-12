# SPDX-License-Identifier: MIT
"""USB MV, RM, RMDIR and LS on a real FAT image (core/fatput.c), plus macOS dot-file entries."""
import mmap, struct, unittest
from test_m1_user import ProtocolBase, fsck, text

def lfn_dot(name_first=b'.\0'):
    e=bytearray(32);e[0]=0x41;e[11]=0x0f;e[1:3]=name_first;e[3:11]=b'S\0p\0o\0t\0';return bytes(e)

class UsbFs(ProtocolBase):
    def cmd(self,line):
        self.c.feed(b'INFO\n',5);self.run_protocol(3);self.c.take_output()
        b=line.encode()+b'\n';self.c.feed(b,len(b));out=self.run_protocol(50)
        return out
    def seed(self,*paths):
        for p in paths: self.assertEqual(self.c.fatput(p.encode(),p.encode()*300,len(p)*300),0,text(self.c,'fatput_fail'))
    def clean(self):
        r=fsck(self.disk,self.work);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertNotIn('Fix',r.stdout,r.stdout)
    def gone(self,path): self.assertEqual(self.c.asset_open(path.encode(),len(path),0),-2)

    def test_rename_in_folder(self):
        self.seed('GAMES/DICE/DICE.GAM','GAMES/DICE/DICE.PAL');free=self.free()
        self.assertIn('OK mv /GAMES/DICE/DICE.PAL /GAMES/DICE/DICE2.PAL',self.cmd('MV GAMES/DICE/DICE.PAL GAMES/DICE/DICE2.PAL'))
        self.assertEqual(self.read('/GAMES/DICE/DICE2.PAL'),b'GAMES/DICE/DICE.PAL'*300);self.gone('/GAMES/DICE/DICE.PAL')
        self.assertEqual(self.free(),free);self.clean();self.untouched()
    def test_move_file_between_folders(self):
        self.seed('GAMES/DICE/DICE.GAM','GAMES/OTHR/X.GAM');free=self.free()
        self.assertIn('OK mv',self.cmd('MV GAMES/DICE/DICE.GAM GAMES/OTHR/DICE.GAM'))
        self.assertEqual(self.read('/GAMES/OTHR/DICE.GAM'),b'GAMES/DICE/DICE.GAM'*300);self.gone('/GAMES/DICE/DICE.GAM')
        self.assertEqual(self.free(),free);self.clean();self.untouched()
    def test_move_folder_fixes_dotdot(self):
        self.seed('GAMES/DICE/DICE.GAM','APPS/HELLO/MAIN.JS')
        self.assertIn('OK mv',self.cmd('MV GAMES/DICE OS/APPS/DICE'))
        self.assertEqual(self.read('/OS/APPS/DICE/DICE.GAM'),b'GAMES/DICE/DICE.GAM'*300);self.gone('/GAMES/DICE/DICE.GAM')
        self.clean();self.assertIn('OK mv',self.cmd('MV OS/APPS/DICE GAMES/BACK'));self.clean()
        self.assertEqual(self.read('/GAMES/BACK/DICE.GAM'),b'GAMES/DICE/DICE.GAM'*300);self.untouched()
    def test_rm_file_and_rmdir(self):
        free=self.free();self.seed('GAMES/DICE/DICE.GAM','GAMES/DICE/DICE.PAL')
        self.assertIn('ERR MOV NOT EMPTY',self.cmd('RMDIR GAMES/DICE'))
        self.assertIn('ERR MOV NOT FILE',self.cmd('RM GAMES/DICE'))
        self.assertIn('OK rm /GAMES/DICE/DICE.GAM',self.cmd('RM GAMES/DICE/DICE.GAM'));self.gone('/GAMES/DICE/DICE.GAM')
        self.assertIn('ERR MOV MISSING',self.cmd('RM GAMES/DICE/DICE.GAM'))
        self.assertIn('ERR MOV NOT DIR',self.cmd('RMDIR GAMES/DICE/DICE.PAL'))
        self.assertIn('OK rm',self.cmd('RM GAMES/DICE/DICE.PAL'))
        self.assertIn('OK rmdir /GAMES/DICE',self.cmd('RMDIR GAMES/DICE'))
        self.clean();self.assertEqual(self.free(),free-1)   # only /OS... the GAMES folder itself stays
        self.untouched()
    def test_refusals_touch_nothing(self):
        self.seed('GAMES/DICE/DICE.GAM','GAMES/OTHR/X.GAM','OS.ELF')
        for line,err in (('RM KERNA.BIN','MOV ZONE'),('RM BOOTCT0.BIN','MOV ZONE'),('MV KERNA.BIN GAMES/K.BIN','MOV ZONE'),
                         ('MV GAMES/DICE/DICE.GAM KERNA.BIN','MOV ZONE'),('RM OS/CACHE/GAMES.IDX','MOV ZONE'),('RM OS/SAVES/X/Y','MOV ZONE'),
                         ('RMDIR GAMES','MOV ZONE'),('RMDIR OS/APPS','MOV'),('RM OS','MOV ZONE'),('RMDIR OS/SAVES','MOV NOT DIR'),
                         ('RM ../KERNA.BIN','MOV PATH'),('RM /GAMES/X','MOV PATH'),('RM games/dice','MOV PATH'),('RM GAMES//X','MOV PATH'),
                         ('MV GAMES/DICE GAMES/DICE/SUB','MOV SELF'),('MV GAMES/DICE GAMES/DICE','MOV SELF'),
                         ('MV GAMES/DICE/DICE.GAM GAMES/OTHR/X.GAM','MOV EXISTS'),('MV GAMES/NONE/A GAMES/B','MOV MISSING'),
                         ('MV GAMES/DICE/DICE.GAM GAMES/NONE/A','MOV MISSING'),('MV GAMES/DICE OS/DICE','MOV NOT FILE'),
                         ('RM GAMES/A.B.C','MOV PATH'),('MV GAMES/DICE','command'),('RM','command')):
            with self.subTest(line): self.assertIn('ERR '+err,self.cmd(line))
        self.assertEqual(self.read('/GAMES/DICE/DICE.GAM'),b'GAMES/DICE/DICE.GAM'*300);self.assertEqual(self.read('/OS/OS.ELF'),b'OS.ELF'*300)
        self.untouched();self.clean()
    def test_not_while_os_runs_or_without_info(self):
        self.seed('GAMES/DICE/DICE.GAM')
        b=b'RM GAMES/DICE/DICE.GAM\n';self.c.feed(b,len(b));self.assertIn('ERR not-ready',self.run_protocol(50))
        self.assertEqual(self.read('/GAMES/DICE/DICE.GAM'),b'GAMES/DICE/DICE.GAM'*300)
    def test_ls_pages_and_root(self):
        self.seed('OS.ELF',*[f'GAMES/DICE/F{i:02d}.GAM' for i in range(40)])
        names=[];at=0
        for _ in range(20):
            out=self.cmd(f'LS GAMES/DICE {at}');names+=[l.split()[4] for l in out.splitlines() if l.startswith('ENT ')]
            tail=out.strip().splitlines()[-1];self.assertTrue(tail.startswith('OK ls /GAMES/DICE next='),out)
            nxt=tail.split('next=')[1].split()[0]
            if nxt=='end': break
            at=int(nxt)
        self.assertEqual(names,[f'F{i:02d}.GAM' for i in range(40)])
        root=self.cmd('LS /');self.assertIn('ENT F ',root);self.assertIn('KERNA.BIN',root);self.assertIn('ENT D 0 10 OS',root)
        self.assertIn('ERR MOV MISSING',self.cmd('LS GAMES/NOPE'))
    def mac_card(self):
        """GAMES/DICE: a dot-named long-name entry before a hidden/system file, an orphan long-name entry, and real files."""
        self.seed('GAMES/DICE/ALFN.GAM','GAMES/DICE/SPOT.GAM','GAMES/DICE/ORPH.GAM','GAMES/DICE/REAL.GAM')
        with open(self.disk,'r+b') as f, mmap.mmap(f.fileno(),0) as m:
            def at(name): i=m.find(name.ljust(8).encode()+b'GAM\x20');assert i>0;return i
            a,sp,o=at('ALFN'),at('SPOT'),at('ORPH')
            m[a:a+32]=lfn_dot();m[sp+11]=0x06                       # SPOT: ".Spot" long name, hidden+system
            m[o:o+32]=bytes([0x41,0x78,0,0x79,0,0,0,0,0,0,0,0x0f])+bytes(20)  # orphan long-name piece, "xy"
    def test_mac_dot_entries_listed_skipped_and_never_break_writes(self):
        self.mac_card()
        out=self.cmd('LS GAMES/DICE');self.assertIn('skipped=1',out);self.assertNotIn('SPOT.GAM',out);self.assertIn('REAL.GAM',out)
        self.assertIn('OK mv',self.cmd('MV GAMES/DICE/REAL.GAM GAMES/DICE/REAL2.GAM'))
        self.assertIn('ERR MOV NOT FILE',self.cmd('RM GAMES/DICE/SPOT.GAM'))      # hidden/system: never removed
        self.assertIn('ERR MOV NOT EMPTY',self.cmd('RMDIR GAMES/DICE'))
        self.assertEqual(self.c.fatput(b'GAMES/DICE/NEW.GAM',b'new',3),0,text(self.c,'fatput_fail'))
        self.assertEqual(self.read('/GAMES/DICE/NEW.GAM'),b'new')
        self.assertIn('OK rm',self.cmd('RM GAMES/DICE/REAL2.GAM'))
        self.assertIn('skipped=1',self.cmd('LS GAMES/DICE'))
    def test_a_dot_folder_in_the_root_and_dot_files_in_games(self):
        """A hidden+system folder entry with a dot long name in the root must not stop PUTF, MV or LS."""
        self.seed('GAMES/DICE/DICE.GAM')
        with open(self.disk,'r+b') as f, mmap.mmap(f.fileno(),0) as m:
            i=m.find(b'GAMES      \x10');self.assertGreater(i,0)
            m[i-32:i]=lfn_dot(b'.\0')                              # long-name piece ".Spot" right before GAMES
        self.assertIn('OK mv',self.cmd('MV GAMES/DICE/DICE.GAM GAMES/DICE/DICE2.GAM'))
        self.assertEqual(self.c.fatput(b'GAMES/DICE/A.GAM',b'a',1),0,text(self.c,'fatput_fail'))
        root=self.cmd('LS /');self.assertIn('skipped=1',root);self.assertNotIn(' GAMES',root)   # the dot-named entry is skipped, not an error

if __name__=='__main__': unittest.main()
