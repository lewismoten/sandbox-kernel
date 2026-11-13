# License texts that ship with JS.ELF

`make -C js bundle` joins these into `NOTICES.TXT` in the card bundle (`tools/bundle.py`).
Anyone who passes on a `JS.ELF` should pass on `NOTICES.TXT` with it.

| File | Covers | Source (fetched 2026-11-13) | SHA-256 |
| --- | --- | --- | --- |
| `SANDBOX-MIT.txt` | Sandbox JS and the sandbox-kernel SDK pieces linked in (crt0, headers) | this repository's `LICENSE` | 05fabe2bb19767ff0e974e2b084a7693d4436143462a36dfad53e5d05f93306e |
| `QUICKJS-NG.txt` | QuickJS-ng 0.18.0 | `js/third_party/quickjs-ng/LICENSE` (pinned, unchanged) | 96f73f9d2a16c21a36b418f06073be26e7d6d5e7c1bc99756b21a4f2c74ef171 |
| `NEWLIB.txt` | newlib 4.6.0 `libc.a` and `libm.a` (linked, from the Arm GNU Toolchain 15.3.rel2) | https://sourceware.org/git/?p=newlib-cygwin.git;a=blob_plain;f=COPYING.NEWLIB;hb=refs/tags/newlib-4.6.0 | f3afe48e4bc6ed8466a42e9dacb6be1d8f9cbf5aac15cb8e474a5ccde8b40ef6 |
| `GCC-RUNTIME-EXCEPTION.txt` | libgcc (`-lgcc`): the GCC Runtime Library Exception 3.1 | https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-15.2.0/COPYING.RUNTIME | 9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74 |
| `GPL-3.0.txt` | the license libgcc is under, which the exception modifies | https://www.gnu.org/licenses/gpl-3.0.txt | 3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986 |
| `FONT-AND-GIF.txt` | the 5x9 font and the GIF decoder | this project (MIT) | n/a |

Notes: the toolchain build is 15.3.1; the exception text is the version 3.1 file from GCC 15.2.0
(the Arm 15.3 snapshot has no tag in the upstream mirror, and the text has been version 3.1 since
2009). Newlib's text is the file at the 4.6.0 tag, the version the toolchain's `_newlib_version.h`
reports. The toolchain's own documentation folders do not include either text.
`tools/bundle.py` fails if any file here is missing or empty.
