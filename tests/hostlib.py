# SPDX-License-Identifier: MIT
"""One host build of the kernel's pure core (real code, fake media and CDC
from tests/harness.c), shared by the M0 and M1 test files."""
import pathlib, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCES=['core/util.c','core/divide.c','core/store.c','core/protocol.c','core/assets.c','core/fb.c','core/abi.c',
         'core/uspace.c','core/elf.c','core/sys.c','core/os.c','core/fatput.c','tests/harness.c']
def build(out):
    # ELF interposition can otherwise bind kernel error() to glibc's unrelated
    # variadic error(), corrupting calls from protocol.c. Darwin binds locally.
    link = ['-Wl,-Bsymbolic'] if sys.platform.startswith('linux') else []
    subprocess.run(['cc','-shared','-fPIC','-O1','-fno-builtin','-DHOST_TEST','-D_FORTIFY_SOURCE=0','-Wall','-Wextra','-Werror',
                    *link,'-I'+str(ROOT/'tests/stubs'),'-I'+str(ROOT/'include'),*[str(ROOT/s) for s in SOURCES],'-o',str(out)],check=True)
    return out
