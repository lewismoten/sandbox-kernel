# SPDX-License-Identifier: MIT
"""Synthetic NON-AUDIO SDK client; real production paths, portable host flags."""
import sys
import os, pathlib, shlex, subprocess, sys, tempfile, unittest
from test_m1_user import program, ROOT
import fatgen, hostlib

class ArenaNative(unittest.TestCase):
    def test_actual_arena_sdk(self):
        with tempfile.TemporaryDirectory() as work:
            work=pathlib.Path(work);out=work/'arena'
            sources=[{'core/os.c':'tests/supervisor_probe.c','core/uspace.c':'tests/uspace_probe.c'}.get(s,s) for s in hostlib.SOURCES]
            flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('ARENA_SANITIZE') else []
            if flags and sys.platform.startswith('linux'): flags+=['-fno-pie','-no-pie']
            cmd=[os.environ.get('HOSTCC','cc'),'-O1','-g','-fno-builtin','-DHOST_TEST','-D_FORTIFY_SOURCE=0','-Wall','-Wextra','-Werror',*flags,
                 '-I'+str(ROOT/'tests/stubs'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'sdk/include'),*[str(ROOT/s) for s in sources],
                 str(ROOT/'tests/app_arena_native.c'),'-o',str(out)]
            print('$',shlex.join(cmd),flush=True);subprocess.run(cmd,check=True)
            files={'DIAG.ELF':program()}
            supplied=os.environ.get('SUPERVISOR_OLD_ELFS')
            if supplied:
                for path in sorted(pathlib.Path(supplied).glob('*.ELF')):
                    files['OLDIAG.ELF' if path.name=='DIAG.ELF' else path.name]=path.read_bytes()
            disk=work/'card.img';fatgen.build(disk,{'OS':files})
            for name in files:
                cmd=[str(out),str(disk),'/OS/'+name]
                print('$',shlex.join(cmd),flush=True)
                subprocess.run(cmd,check=True,env={**os.environ,'ASAN_OPTIONS':('detect_leaks=0' if sys.platform=='darwin' else 'detect_leaks=1')+':halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})

if __name__=='__main__': unittest.main()
