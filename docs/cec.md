# HDMI-CEC cores (host-only slices)

`drivers/cec_msg.c`, `cec_keys.c` and `cec_sm.c` (headers in `include/`) are pure C: no
registers, no clocks, not in the kernel image yet. `tests/test_cec_core.py` builds them on the
host and runs `tests/cec_core.c` (set `CEC_SANITIZE=1` to opt into ASan/UBSan). Hardware and the input glue come later.

## Transfer completion guarantee

Each successful `cec_sm_next_tx` returns a frame and a nonzero 64-bit transfer ID.
The transport must retain that ID with the original transfer and pass it to
`cec_sm_tx_done`; it must not fetch the current ID when a delayed callback arrives.
Each handoff, including a retry of identical bytes, gets a new ID. Only a completion
matching the current in-flight transfer can change state. A timeout retires that ID
as a transport error, so a delayed success or failure cannot clear a subsequent report or retry,
change the queue, or allow another handoff. Duplicate completions are also ignored.
IDs never wrap: after UINT64_MAX, new handoffs stop until reinitialization.

The transport reports `CEC_SM_TX_ACK`, `CEC_SM_TX_NACK` (an explicit negative
acknowledgement), or `CEC_SM_TX_ERROR` (an uncertain outcome). Only an explicit
poll NACK claims a logical address. A poll timeout or transport error retries the
same candidate up to `CEC_SM_POLL_RETRIES` (two retries, three attempts total).
Exhausting that budget leaves the device unregistered and silent until
reinitialization. An ACK moves to the next candidate with a fresh retry budget.
Non-poll frames retain their single retry on NACK, timeout or transport error.

Before `cec_sm_init` is called again on the same instance, the transport must cancel
or drain all earlier transfers and callbacks. Calls into an instance must be serialized.
This is a software completion guarantee; the transport still owns physical cancellation
or serialization of timed-out hardware sends before starting another hardware send.
The CEC core remains unlinked; these internal C interfaces do not change the kernel/SDK
ABI, its version, syscall records or wire-frame bytes.

The regressions in `tests/cec_core.c` cover a poll at 0 ms, timeout at 1000 ms,
retry handoff, then delayed original poll ACK/NACK. They check that the entire state
is unchanged, no address is claimed, and another send is blocked until the retry's
matching completion. They also cover explicit NACK claims, bounded timeout/error
retries, duplicate completion, and retries with identical bytes and fresh IDs.

## Where the values come from

All opcode, logical-address, operand and UI-command numbers were read from Linux's public
`include/uapi/linux/cec.h` (rpi-6.6.y), whose SPDX line is
`(GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause`; values only, no code taken. libCEC was not
read (GPL-2.0 or commercial). The Linux key numbers in `cec_keys.c` come from
`input-event-codes.h`; they are the user-space ABI the kernel's `/dev/input` already uses.

Verified there: opcodes 0x00 Feature Abort, 0x04 Image View On, 0x36 Standby, 0x44/0x45 User
Control Pressed/Released, 0x46/0x47 Give/Set OSD Name, 0x82 Active Source, 0x83/0x84 Give/Report
Physical Address, 0x85 Request Active Source, 0x86 Set Stream Path, 0x87 Device Vendor ID,
0x8C Give Device Vendor ID, 0x8F/0x90 Give/Report Power Status, 0x9E/0x9F CEC Version/Get;
logical addresses TV 0, playback 4/8/11, 15 broadcast/unregistered; playback device type 4;
CEC version 1.4 = 5; power on = 0; abort reasons (0 unrecognized); max frame 16 bytes;
UI commands Select 0 .. Right 4, diagonals 5-8, root menu 9, setup menu 0x0A, 0x0D (Linux names
it BACK), numbers 0x20-0x29, Enter 0x2B, Clear 0x2C, channel up/down 0x30/0x31, volume 0x41/0x42,
F1-F4 blue/red/green/yellow 0x71-0x74.

## Not verified (no citable source read)

- That the CEC 1.4 spec table calls 0x0D "Exit"; Linux calls it BACK. The mapping (KEY_ESC) is
  the same either way.
- The 550 ms release timeout (design-note figure; a real TV's longest repeat gap is untested).
- Replying Feature Abort (reason 0) to Give Device Vendor ID rather than a vendor id; and
  Report Power Status/CEC Version being worth answering (kept because many TVs ask).
- Broadcast ACK inversion, one-shot polling and 1-byte frames on the VC4 block (hardware slice).
- The one retry of non-poll frames, two retries of uncertain polls, the 1 s send
  timeout and the 14-character OSD limit (local policies, untested on hardware).
- Where the key mapping puts Root menu (KEY_HOME), Setup menu (KEY_MENU), channel keys and
  colors (policy, not spec).
