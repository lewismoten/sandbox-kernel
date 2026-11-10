# The OS serial channel (`/dev/serial0`, ABI 1.10)

A running OS needs to tell the host something, and hear something back, while the Mac is driving
it over USB with synthetic input (`KEY`, `AXIS`). Until now an OS had `/dev/log`, one-way lines
shown as `LOG OS: ...`. `/dev/serial0` adds a two-way byte stream over the **same USB CDC
link**. Nothing else needs to be plugged in.

Only USB CDC carries it. (A UART path could be added later for a board whose GPIO header is free;
the Pi Zero W here has its header covered by the e-ink HAT, so none exists.)

## For the OS (`sdk/include/sandbox/abi.h`)

- `open("/dev/serial0", access | SBX_O_NONBLOCK)`, choosing one access mode:
  `SBX_O_RDONLY`, `SBX_O_WRONLY` or `SBX_O_RDWR`. Blocking
  opens return `EINVAL`; a second open while one is held returns `EBUSY`. Closing it, or the program
  ending, frees it and empties both buffers.
- `write` takes every byte and never blocks. Output waits in a 512-byte buffer. If it is full, or no
  host has the port open, the bytes that do not fit are **dropped and counted**. The OS is never stalled.
- `read` returns up to 256 buffered bytes from the host, or `EAGAIN` when there are none.
- `sbx_serial_query` (ioctl class 6, op 0) fills `sbx_serial_info`: `transport`
  (`SBX_SERIAL_NONE` 0 or `SBX_SERIAL_USB` 1), `tx_bytes`, `tx_dropped`, `rx_bytes`, `rx_dropped`,
  `tx_pending`, `rx_pending`. The counters count since boot (32-bit internal
  counters wrap); byte totals count accepted bytes, with drops counted separately.
  The current dispatcher does not enforce per-handle read/write direction after open.

The OS channel is hardware-untested. A `USB_ROLE=host` build has no CDC link;
its transport stays `SBX_SERIAL_NONE`, writes drop and empty reads return `EAGAIN`.

## Wire framing (USB)

The kernel's own replies and log lines are lines of text, so OS data is kept as text too and works
in a plain terminal.

OS to host: each line starts with a tag.

- `OS <text>` is OS output that ended with a newline (the newline is not in the text).
- `OS~ <text>` is OS output that did not end there (no newline yet, or 64 bytes taken); the next `OS`
  line continues it.
- Printable ASCII is sent as it is. A backslash is `\\`; every other byte (controls, 128 and up) is
  `\xHH`, so a line is always printable and under 272 bytes.

Host to OS: `TX <len>` and a newline, then exactly `<len>` raw bytes (1 to 64). The kernel replies
`OK tx <accepted>`; bytes that do not fit the 256-byte buffer, or arrive while no program has the
channel open, are dropped and counted. A `TX` needs `INFO` first, like `KEY`.

**TX inactivity timeout:** if the body stops arriving for 2 seconds (measured from the `TX` line or the
last body byte), the kernel abandons the rest, replies `ERR tx-timeout` (a flow-control refusal: no sticky
`ERR:` on the panel), and reads the next bytes as a command again, so a following `STOP` or `INFO` is
answered. The bytes already delivered stay delivered; send the rest in a new `TX`.

Why this: a `LOG` or `OK` line and an `OS` line are all whole lines in one ring, so OS data can never
land inside a kernel log line; `grep '^OS'` separates the two; `TX` works from a terminal with
`printf` and no tool; and the length-prefixed form (not a text line) means the OS can receive
newlines and any byte without escaping.

`INFO` ends with `serial=usb` while a host has the port open and `serial=none` otherwise.

## Host tool

`python3 scripts/usb_kernel.py /dev/cu.usbmodem* console`: arrows, Enter, Space, Q and Esc are
pressed on `/dev/input/event0` (as `key` does); every printable key, Enter and Space is also sent
to the OS as text; OS output shows live; kernel `LOG` lines are dimmed (or start with `# ` when the
output is not a terminal). Ctrl-] quits.

## The Pi remote page (`web/src/piRemote/`, not changed here)

It already opens the same Web Serial port and sorts each line with `classify()` in `protocol.ts`
(`LOG`, `INFO`, `OK`, `ERR`; anything else is `other`). To show OS messages it needs one more case:
a line starting `OS ` or `OS~ ` becomes a new reply kind `os`, shown in the log list
(joining `OS~` continuations), and the text box can send `TX <len>` plus bytes through the same
`SerialLink` writer. Nothing in the kernel changes for that.

## Tests

`tests/test_serial.py` (framing both ways, overflow counting, reader switching, host decoding),
`tests/test_serial_sys.py` (the real call path, `TX`, `INFO`), and `tests/exhaustion_usb.c`
(OS lines and `LOG` lines in one ring, never mixed).
