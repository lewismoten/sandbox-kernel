#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the card bundle: a folder whose contents you copy to the root of an SD card.
Usage: bundle.py <build folder with JS.ELF> <out folder>   (make -C js bundle)

  OS/JS.ELF  OS/AUTORUN.TXT (JS.ELF, then APPS/TINYOS)  OS/APPS/{TINYOS,HELLO,CLOCK}/...
  NOTICES.TXT (the license texts in LICENSES/, joined)  README.TXT (what this is, 8.3 names, plain text)

Every name is 8.3 upper case; the bundle is checked with app_check.py --card and fails on a missing license."""
import hashlib, pathlib, re, shutil, subprocess, sys

JS = pathlib.Path(__file__).resolve().parents[1]
APPS = {"TINYOS": JS / "examples/TINYOS", "HELLO": JS / "apps/HELLO", "CLOCK": JS / "apps/CLOCK"}
# Manifests the card copies get (the repo's HELLO and CLOCK stay legacy apps: the host fixtures pin their log lines).
MANIFESTS = {
    "HELLO": '{"name": "Hello", "version": "0.1.0", "api": 1, "capabilities": ["display", "input", "fs.read", "apps.launch"], "description": "A small page-shaped app; Esc returns to the launcher."}\n',
    "CLOCK": '{"name": "Clock", "version": "0.1.0", "api": 1, "capabilities": ["display", "input"], "description": "An uptime clock page."}\n',
}
SKIP = {"README.md", "screenshots"}  # not part of an app on the card
LICENSES = [  # (file, what it covers)
    ("SANDBOX-MIT.txt", "Sandbox JS (this runtime, the example apps) and the sandbox-kernel SDK pieces linked in"),
    ("QUICKJS-NG.txt", "QuickJS-ng 0.18.0 (the JavaScript engine inside JS.ELF)"),
    ("NEWLIB.txt", "newlib 4.6.0 libc and libm, linked from the Arm GNU Toolchain 15.3.rel2"),
    ("GCC-RUNTIME-EXCEPTION.txt", "libgcc (linked): the GCC Runtime Library Exception 3.1"),
    ("GPL-3.0.txt", "the GNU General Public License 3, which libgcc is under and the exception modifies"),
    ("FONT-AND-GIF.txt", "the 5x9 bitmap font and the GIF decoder"),
]
# APP.JSON is the one name that is not strict 8.3 (a four-letter extension, stored as a long name); the runtime reads it by that name.
NAME83 = re.compile(r"[A-Z0-9_-]{1,8}(\.[A-Z0-9_-]{1,3})?|APP\.JSON")

def version():
    return (JS / "VERSION").read_text().strip()

def notices(ver):
    out = [f"NOTICES for JS.ELF {ver} (Sandbox JS)\n\nJS.ELF contains the software below. Each license text follows, unchanged.\nThis file is UTF-8 text.\n"]
    for f, what in LICENSES:
        p = JS / "LICENSES" / f
        if not p.is_file() or p.stat().st_size < 100:
            sys.exit(f"missing or empty license text: {p}")
        out.append("=" * 72 + f"\n{what}\n({f})\n" + "=" * 72 + "\n\n" + p.read_text(encoding="utf-8").rstrip() + "\n")
    return "\n".join(out)

def readme(ver, sha):
    return f"""SANDBOX JS {ver}: card bundle
==============================

What this is: JS.ELF (the Sandbox JS runtime, QuickJS-ng inside) and three
apps written in JavaScript: TINYOS (a small home screen with files and
settings), HELLO and CLOCK. It runs on a sandbox-kernel with ABI 1.12 or newer.

JS.ELF sha256: {sha}

NOT TESTED ON A BOARD. Everything here passed the host tests only.

Copy to the card
  Copy the OS folder and NOTICES.TXT to the root of the SD card. If the card
  already has an OS folder (GMOS.ELF lives there), merge: do not delete it.
  OS/AUTORUN.TXT is what makes the board start JS.ELF and then TINYOS.

Go back
  Edit OS/AUTORUN.TXT so its first line is GMOS.ELF (or delete the file; the
  kernel then starts GMOS.ELF if it is there).

Files
  OS/JS.ELF           the runtime
  OS/AUTORUN.TXT      line 1 JS.ELF, line 2 APPS/TINYOS
  OS/APPS/TINYOS/     the home screen (settings are saved in OS/SAVES/TINYOS)
  OS/APPS/HELLO/      a small page-shaped app (Esc returns to TINYOS)
  OS/APPS/CLOCK/      an uptime clock (Esc or Q leaves it for the picker)
  NOTICES.TXT         license texts for what is inside JS.ELF; keep it with JS.ELF
"""

def build(build_dir, out):
    elf = pathlib.Path(build_dir) / "JS.ELF"
    if not elf.is_file():
        sys.exit(f"no {elf}: run make -C js first")
    ver = version()
    if out.exists():
        shutil.rmtree(out)
    (out / "OS/APPS").mkdir(parents=True)
    shutil.copy(elf, out / "OS/JS.ELF")
    (out / "OS/AUTORUN.TXT").write_text("JS.ELF\nAPPS/TINYOS\n")
    for name, src in APPS.items():
        for f in sorted(src.rglob("*")):
            rel = f.relative_to(src)
            if rel.parts[0] in SKIP or f.is_dir():
                continue
            if not NAME83.fullmatch(f.name) or len(rel.parts) != 1:
                sys.exit(f"{name}/{rel}: not a flat 8.3 upper-case name")
            (out / "OS/APPS" / name).mkdir(exist_ok=True)
            shutil.copy(f, out / "OS/APPS" / name / f.name)
    for name, text in MANIFESTS.items():
        if not (out / "OS/APPS" / name / "APP.JSON").exists():
            (out / "OS/APPS" / name / "APP.JSON").write_text(text)
    (out / "NOTICES.TXT").write_text(notices(ver), encoding="utf-8")
    (out / "README.TXT").write_text(readme(ver, hashlib.sha256(elf.read_bytes()).hexdigest()))
    for f in out.rglob("*"):
        if not NAME83.fullmatch(f.name):
            sys.exit(f"{f.relative_to(out)}: not an 8.3 upper-case name")
    r = subprocess.run([sys.executable, str(JS / "tools/app_check.py"), "--card", str(out)], capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode:
        sys.exit("app_check --card failed:\n" + r.stderr)
    n = sum(1 for f in out.rglob("*") if f.is_file())
    print(f"bundle {out}: {n} files, JS.ELF {ver}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], pathlib.Path(sys.argv[2]))
