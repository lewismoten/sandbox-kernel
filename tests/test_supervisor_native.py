# SPDX-License-Identifier: MIT
"""Standalone real-path native/sanitizer execution, no ctypes ASan preload."""
import os, pathlib, shlex, subprocess, sys, tempfile, unittest
from test_m1_user import program, ROOT
import fatgen, hostlib

class SupervisorNative(unittest.TestCase):
    def test_actual_supervisor_and_loader(self):
        with tempfile.TemporaryDirectory() as work:
            work=pathlib.Path(work);out=work/'supervisor'
            sources=[{'core/os.c':'tests/supervisor_probe.c','core/uspace.c':'tests/uspace_probe.c'}.get(s,s) for s in hostlib.SOURCES]
            flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('SUPERVISOR_SANITIZE') else []
            if flags and sys.platform.startswith('linux'): flags+=['-fno-pie','-no-pie']
            cmd=['cc','-O1','-g','-fno-builtin','-DHOST_TEST','-D_FORTIFY_SOURCE=0','-Wall','-Wextra','-Werror',*flags,
                 '-I'+str(ROOT/'tests/stubs'),'-I'+str(ROOT/'include'),*[str(ROOT/s) for s in sources],
                 str(ROOT/'tests/supervisor_native.c'),'-o',str(out)]
            print('$',shlex.join(cmd),flush=True);subprocess.run(cmd,check=True)
            files={'DIAG.ELF':program()}
            supplied=os.environ.get('SUPERVISOR_OLD_ELFS')
            if supplied:
                for path in sorted(pathlib.Path(supplied).glob('*.ELF')):
                    # Keep synthetic DIAG for the SVC/fault fixture; old DIAG has its own name.
                    files['OLDIAG.ELF' if path.name=='DIAG.ELF' else path.name]=path.read_bytes()
            disk=work/'card.img';fatgen.build(disk,{'OS':files})
            for name in files:
                cmd=[str(out),str(disk),'/OS/'+name]
                print('$',shlex.join(cmd),flush=True)
                subprocess.run(cmd,check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})

if __name__=='__main__': unittest.main()
