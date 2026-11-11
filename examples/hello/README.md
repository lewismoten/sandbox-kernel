# HELLO: a tiny OS to copy

`main.c` (about 110 lines) is a complete OS program for sandbox-kernel. It checks the ABI
version, draws "Hello, world!" and an orange box on `/dev/fb0` (requested 960x540, 16-bit color, rows
`pitch` bytes apart; drawing uses the negotiated dimensions), moves the box with the arrow keys from `/dev/input/event0`, sends the
heartbeat every frame, and exits when you press Esc. The Makefile uses only the SDK
(`sdk/lib/crt0.S`, `mem.c`, `sandbox.ld`, `sdk/include/sandbox/`). The 5x7 letters are drawn
from a small table of bitmaps drawn for this example in `main.c` (MIT, like everything here).
Copy this folder, rename it, and change `main.c`.

## Build

You need the Arm GNU `arm-none-eabi` toolchain (set `TOOLCHAIN=...-` if it is elsewhere).

```
make -C examples/hello          # HELLO.ELF, about 7 KB
make examples                   # hello and clock, from the repository root
make test                       # also builds them and checks the ELF rules and the 8 MiB quota
```

## Put it on the Pi

Over USB (the kernel's recovery console):

```
scripts/usb_kernel.py <port> stop                         # cancel autostart/stop a running OS
scripts/usb_kernel.py <port> put examples/hello/HELLO.ELF    # installs /OS/HELLO.ELF
scripts/usb_kernel.py <port> run HELLO.ELF                   # starts it and shows the log
scripts/usb_kernel.py <port> key right right down esc        # fake key presses
```

Or on the card: copy `HELLO.ELF` to `/OS/HELLO.ELF`, and put `HELLO.ELF` on the first line of
`/OS/AUTORUN.TXT` to start it at boot (see section 2 of [docs/os-authors.md](../../docs/os-authors.md)).

## What to expect

A dark blue screen, "Hello, world!" in big white letters, and an orange box. Arrow keys move
the box (it stops at the edges). Esc logs `HELLO bye` and returns to the recovery console.
On a kernel older than ABI 1.2 it exits with status 2 and no picture.

## E-ink (not enabled yet)

The e-ink API is implemented in ABI 1.9: `/dev/eink`, title/query/clear ioctls
and a 3,904-byte page write ([docs/eink.md](../../docs/eink.md)). HELLO does not
call it: `main.c` still has an empty `#if SBX_ABI_HAS_EINK` placeholder and a stale
TODO; the SDK does not define that macro. Defining it alone adds no behavior.
To extend the example, check startup major 1/minor >=9 and follow the
[health-servicing title/page example](../../docs/os-authors.md#show-your-os-on-the-e-ink-abi-19).
