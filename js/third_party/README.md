# Third-party code in JS.ELF

| What | Where | License | Notice |
| --- | --- | --- | --- |
| QuickJS-ng v0.18.0 | `quickjs-ng/` (pinned, unchanged) | MIT | `quickjs-ng/LICENSE` |
| newlib `libc.a`, `libm.a` (linked, not copied) | the Arm GNU Toolchain 15.3.rel2 (`arm-none-eabi`) | BSD-style and similar permissive licenses; `libm` from Sun's fdlibm | [COPYING.NEWLIB](https://sourceware.org/newlib/COPYING.NEWLIB) |
| `libgcc.a` (linked) | the same toolchain | GPL-3.0-or-later with the GCC Runtime Library Exception | [gcc.gnu.org/onlinedocs/gccint/Libgcc.html](https://gcc.gnu.org/onlinedocs/gccint/Libgcc.html), [the exception](https://www.gnu.org/licenses/gcc-exception-3.1.html) |
| The 5x9 bitmap font (`../src/font_data.c`) | built from `../fonts/font.txt` | MIT | capitals, digits and `- : . /` from sandbox-kernel's 5x7 status font (same author); lowercase and punctuation drawn new |

The toolchain installed here does not ship the newlib license text, so it is linked above and
not copied: a release that ships `JS.ELF` must carry COPYING.NEWLIB's notices and the
libgcc exception text next to it (open item for slice 6, the release path). `gif.c` is a C port
of this project's `formats/gif.ts` (MIT), itself from Game-Maker's GIFC.C behavior.
