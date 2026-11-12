# HDMI-CEC

Current status: linked into the runtime image (device and host roles), host-tested against a fake
register file, and run on a bench board (PI-1) with no TV (see "Bench result" below).
PI-2 subsequently reached EDID physical address 2.0.0.0 but timed out on every claim poll.
The TV test procedure is
`.for-lewis/research/pi/2026-11-10-cec-tv-test.md` (the maintainer's notes, outside this repository).

## Layers

| File | Job |
| --- | --- |
| `drivers/cec_msg.c`, `cec_keys.c`, `cec_sm.c` | frames, UI command to key with repeat and a 550 ms release, address claim and replies (pure) |
| `drivers/cec_regs.c`, `include/cec_regs.h` | the HDMI core's CEC register offsets and fields; divider and bit-timing words for an HSM clock |
| `drivers/cec_drv.c` | transport over a read/write pair: programs the block, sends the state machine's frames, handles a received frame or a finished send, cancels a stuck send, logs |
| `drivers/cec_edid.c` | the physical address from the display's HDMI vendor block in its EDID |
| `drivers/cec_log.c` | the bounded session log (24 KiB: the first 6 KiB of the session always kept, then the newest lines) |
| `drivers/cec_hw.c` | the real registers (HDMI core at 0x20902000), HSM clock from the clock manager, the EDID from the firmware mailbox, key events into `/dev/input/event0`, `CEC.TXT` |

`tests/test_cec_core.py` and `tests/test_cec_drv.py` build them on the host (set `CEC_SANITIZE=1`
for the first to opt into ASan/UBSan). The second drives `cec_drv.c` with a fake chip: divider 4092
for the 163.68 MHz HSM clock, the threshold words, a claim after a poll that nobody answers, the
announcement, received keys and replies, a stuck send, and the log bound.

## What happens on the board

1. Boot: `config.txt` carries `hdmi_ignore_cec=1` and `hdmi_ignore_cec_init=1`
   (`scripts/assemble_boot.py`). The firmware's own CEC task otherwise claims a playback address,
   answers the TV with the name "raspberry" and sends its own Active Source, so two devices would
   fight over one address. `hdmi_ignore_cec=1` is the switch that hands the bus over (the
   firmware then behaves as if the TV had no CEC); `hdmi_ignore_cec_init=1` is kept as well so no
   start-up Active Source can come from the firmware even if a future firmware version honors only
   that one. Whether the firmware really leaves the registers alone is confirmed only on a TV.
2. After HDMI is up (`cec_hw_init`, next to `hdmi_audio_init`): the divider is `hsm / 40000`
   (4092 at the 163.68 MHz measured on 2026-11-09), the thresholds are the microsecond values
   divided by the 25 us tick, the address is 15 (unregistered).
3. The physical address comes from the EDID (mailbox tag 0x00030020, CEA extension, HDMI vendor
   block). If the EDID cannot be read the kernel assumes 1.0.0.0 and says so in the log.
4. A poll to address 4 (then 8, then 11): ACK means occupied; NACK or a 1 s timeout
   without ACK means free under this driver's claim policy, so it is claimed (written
   to the register's address field, so the block acknowledges frames sent to it). Then, in
   order: Report Physical Address (broadcast), Set OSD Name "Game Maker" (directed to the TV; the spec has no broadcast form), Image View On to
   the TV, Active Source. The initial sequence happens at claim time, usually
   before OS autostart. Wake can queue Image View On and Active Source again;
   standby queues a directed Standby to TV address 0 ([power-saving.md](power-saving.md)).
5. Replies: Give OSD Name, Give Physical Address, Give Device Power Status (on), Get CEC Version
   (1.4), Menu Request (Menu Status: activated), Request
   Active Source and Set Stream Path once active. Menu Request is answered Menu Status
   "activated", so a TV that withholds the remote from a device with no active menu keeps
   sending keys. Active Source (on announce and on Set Stream Path to us) is followed by an
   unsolicited directed Menu Status "activated" for TVs that wait for it. Give Device Vendor ID gets a broadcast Device Vendor ID of 000000 (no vendor
   claimed; the value is a convention, not a registered id). Anything else directed at us gets
   Feature Abort "unrecognized opcode" (Feature Abort itself is ignored). These messages request TV input selection; they do
   not guarantee that a TV changes input.
6. Keys: User Control Pressed goes through `cec_keys` to `input_frame` (the same queue the USB
   keyboard and the synthetic device use): Select and Enter are Enter, arrows are arrows, Exit is
   Esc, Play, Pause, Play Function and Pause-Play Function are Space (Record Function is not passed on), 0 to 9, Clear, channel up and down, the four colors (F1 to F4), Root menu is Home, Setup menu
   is Menu. A remote's repeat (Pressed again about every 100 to 500 ms) becomes a key repeat for
   arrows and channel up/down only; Select, Exit and the others act once per press. No Pressed for
   550 ms ends the press when no Released arrives. If the USB keyboard left, `input_add()` brings the device back (host role).
7. Broadcast frames are acknowledged by silence, so the chip's "good" flag says nothing about
   them: the driver never retries a completed broadcast even if GOOD is clear.
   A software timeout or transport
   error still permits the state machine's one retry. The log records the raw
   `ack`/`nack` result for completed frames; see the bench evidence below.

Once claimed, valid directed User Control Pressed (including unmapped UI
codes) and operand-free Image View On reset idle power activity. A broadcast Active Source from
another device does not: it neither wakes the Pi nor makes it take the input back. Initiator 15 is ignored. A pending standby
or wake intent waits for a claimed address and queue room, with the newest intent
replacing the older one. No raw CEC syscall or device is exposed.

## The log

`cec_log` keeps the session in RAM and `cec_hw` writes it to `/OS/SAVES/KERNEL/CEC.TXT` (the saves
area, so GMOS and the card tools see a normal file): first write 8 s after CEC initialization, then one
every 10 s for the first 10 minutes and one a minute after that (up to 400 writes), only when something was logged, and never while a USB install is running. It records the init values
(physical address, HSM clock, divider), every poll and frame sent with `ack` or `nack`, every
received frame that is not a held key, each new key press (`UI 01 key 103`, or `UI 66 UNMAPPED`
for a code the map ignores), send timeouts and the claim result. Each line starts with
milliseconds since the first CEC clock call during initialization.

Over USB: `python3 scripts/usb_kernel.py <port> ceclog` prints the newest 12 lines (the kernel
replays them to a new reader about a second after it connects), so a session can be read afterward
without pulling the card.

## Bench result (2026-11-10, PI-1, a display with no CEC, no TV)

What the real block did, read from `ceclog` of a trial boot:

- The registers are where Linux says (HDMI core 0x20902000 + 0xE8..): the divider 4092 and thresholds were
  accepted (read back in the log), and `CPU_STATUS` bit 6 (read raw, unmasked) raises for each finished send,
  so polling works. `CPU_STATUS` also shows bit 1 constantly; it is ignored.
- A poll to address 4 is reported `nack` (nobody there), so address 4 was claimed; one-byte frames work.
- Broadcasts (Report Physical Address, Set OSD Name, Active Source) report `ack` (good), so the chip's flag
  is already inverted for broadcast: nothing NACKed them. Directed frames to the TV (address 0) report `nack`
  because nobody is there. That is the expected answer for an empty bus.
- After a directed `nack` the block was not ready for the next frame at once: a frame started right away never
  completed (timeout after 1 s, every later send too). The driver now waits 250 ms (`CEC_DRV_HOLD_MS`) before
  the next send after a failed directed send; with that, the retry and the frames after it completed. Polls
  and broadcasts need no wait. The block seems to be on its own retry for a moment (this is an inference).
- The GMOS start changes the video mode; the driver checks the divider, address field and a threshold word every
  100 ms and writes everything again if they changed (`registers programmed again` in the log). That has not
  been seen to trigger yet; it is there because a mode change may wipe the block.
- `CEC.TXT` was written (`CEC.TXT written` in the USB log) and the kernel confirmed its trial after 30 s.
- Never exercised on this bench: any received frame (`rx 0`), so the RX path (read of the RX words, receive
  clear) and the key events are tested only against the fake chip until a TV is connected.

## A TV that starts late (2026-11-12, Insignia Fire TV)

A Fire TV takes a long time to boot; the Pi is ready at once. On that TV every send in the first 11 s ended with
no completion at all (`CPU_STATUS` bit 6 never rose, `st 00000002` is only the ignored bit 1), not an ACK or a NACK.
The driver now never gives up:

- A poll that times out claims the address only tentatively (`cec_sm.tentative`): the register takes it so frames to us are
  answered, but the claim is checked again (a fresh poll) as soon as any transfer completes. A real NACK is the only proof it is free.
- While nothing has ever completed (`bus_ok` 0), the whole claim starts again after 2 s, 4 s, 8 s ... up to 30 s apart (registers
  written again, queue cleared, poll, announce). It stops backing off the moment a frame completes.
- HPD (HDMI core HOTPLUG, bit 0) is read every pass: a rising edge reads the EDID again (`physical address now ...` and a restart
  if it changed). If the boot EDID was unreadable, it is retried every 3 s. HPD is only logged and used as a trigger, never as a gate:
  AUDIO.TXT read hotplug=0 with a working EDID, so the bit's meaning is unverified.
- A Report Power Status "on" or "standby to on" from the TV while we are the active source repeats Active Source and Menu Status.
  Request Active Source, Give Physical Address, Give OSD Name and Give Device Power Status were already answered whenever they arrive.
- CEC.TXT: a `session boot <boot id> hpd N` line, `hotplug rose/fell`, `hpd` on every register line, and a raw `done; c1 c5 st hpd`
  line for each of the first 16 completed sends. CEC.TXT holds one boot (the file is replaced each save), times are ms since boot.
  Seconds 600 and later in a log with no TV are the 10-minute idle Standby (`power.c`), not a retry.

## No receive and no completions (2026-11-12, second Insignia session)

Evidence (`CEC.TXT` of that session; the old 2 KiB log held only the last 15 s of it, which is why its first lines and the
`session boot` line were gone, and it was written at most every 30 s): sends timed out one after another, one 12-byte
directed frame completed and was ACKed, `alive tx 3/4 rx 0`, and `c5` read `00CCBCB4` (bit 23, the receive-event flag) at some
moments while `CPU_STATUS` showed only the ignored bit 1. Linux (rpi-6.6.y `vc4_hdmi.c`, facts only) differs from this driver in two ways:
it writes the CEC bit to `CPU_MASK_CLEAR` (0x354) when CEC is enabled (the driver never did), and it consumes both TX end and RX
frames from the one status bit, so a missed bit is a missed frame. Changes, all in `cec_drv.c`:

- the CEC bit is unmasked at every (re)program and every 100 ms check (`CPU_MASK_CLEAR`);
- besides `CPU_STATUS` bit 6, a send is also seen to end when `c1` bit 31 (TX_EOM) rises after a start that did not already show it,
  and a frame is read when `c5` bit 23 or `c1` bit 29 (RX_EOM) is set with the status bit clear (not while sending; at most every
  30 ms; switched off with a log line if the flag survives the receive clear or eight reads in a row give no frame);
- a send timeout resets the transmitter only (never the receiver) and, if a receive flag is up, reads that frame and re-arms the receiver
  (`CLEAR_RECEIVE_OFF`) first;
- the send timeout is 2 s (a 16-byte frame with the block's own retry is over a second on the wire);
- log: every received frame as `rx raw <n>B <bytes> via status|flags|timeout c1 <word>`, a `flags;` line (c1 c5 st mask hotplug) whenever
  the status, RX flag, EOM or GOOD bits change (at most every 250 ms), `mk` (CPU_MASK_STATUS), raw HOTPLUG (`hp`) and HOTPLUG_INT (`hi`)
  on every register line.

Hotplug: the offset 0x00c and bit 0 match Linux (`HDMI_HOTPLUG`, `VC4_HDMI_HOTPLUG_CONNECTED`), so the read is probably not at a wrong
address; the firmware may drive HDMI without setting it (inferred). `hp` and `hi` in the log show the whole words.

Frames for other addresses (the TV talking to an audio system at 5 with ARC on) are ignored without any send; only directed frames
for our address and the broadcasts listed above are answered.

## Limits and things the TV session decides

- The chip is polled from the main loop, not by interrupt. A long SD write or USB install can delay
  a poll; the received frame stays in the block until read. If frames are lost, an interrupt is the next step.
- Receive on silicon, and whether `hdmi_ignore_cec=1` really keeps the firmware's CEC task off (the bench card
  still has the old `config.txt`; see the TV test notes).

## Transfer completion guarantee

Each successful `cec_sm_next_tx` returns a frame and a nonzero 64-bit transfer ID.
The transport must retain that ID with the original transfer and pass it to
`cec_sm_tx_done`; it must not fetch the current ID when a delayed callback arrives.
Each handoff, including a retry of identical bytes, gets a new ID. Only a completion
matching the current in-flight transfer can change state. A timeout retires that ID,
so a delayed success or failure cannot clear a subsequent report or retry,
change the queue, or allow another handoff. Duplicate completions are also ignored.
IDs never wrap: after UINT64_MAX, new handoffs stop until reinitialization.

The transport reports `CEC_SM_TX_ACK`, `CEC_SM_TX_NACK`, `CEC_SM_TX_TIMEOUT`, or
`CEC_SM_TX_ERROR`. A poll ACK advances to the next candidate; NACK or timeout
claims the current candidate. ERROR (for example, registers overwritten during a
send) retries that candidate up to `CEC_SM_POLL_RETRIES` (two retries), then leaves
us unregistered. Non-poll frames retain their single retry on NACK, timeout or error.

On timeout the hardware driver clears START_XMIT, pulses TX reset, clears the stale
event and waits 250 ms before sending queued reports. It services a pending event
before checking the deadline, so an ACK latched during a long main-loop pass wins.
Each completed or timed-out poll logs one outcome: `poll 4 ack`, `poll 4 nack`,
or `poll 4 timeout`. A register overwrite is logged as a transport error through
the register-change/reprogram messages instead.

Before `cec_sm_init` is called again on the same instance, the transport must cancel
or drain all earlier transfers and callbacks. Calls into an instance must be serialized.
This is a software completion guarantee; the transport still owns physical cancellation
or serialization of timed-out hardware sends before starting another hardware send.
These internal C interfaces do not change the kernel/SDK
ABI, its version, syscall records or wire-frame bytes.

The host regressions cover ACK/NACK/timeout at all three playback addresses,
bounded transport-error retries, stale and duplicate completions, non-poll timeout
retries, reset before sending reports, and an ACK/NACK pending at the deadline.
Divider tests include the measured 163682864 Hz and both ends of the 12-bit range.

## PI-2 claim failure and public register comparison (2026-11-10)

The supplied PI-2 log had `c1 0010FFFC`, `c5 004CBCB4`, `st 00000002`,
followed by three 1 s timeouts of the same candidate. The old state machine treated
all three as transport errors and went silent; it never tried addresses 8 or 11.
The timeout claim policy above fixes that decision. It does not establish why
this board failed to signal completion; a new board session is still needed to
verify announcements and received remote keys.

The [public VC4 register definitions](https://gem5.googlesource.com/arm/linux/+/93cc1228b4a60584f3dadbd84f19f365bb945acb/drivers/gpu/drm/vc4/vc4_regs.h)
document CNTRL_1 at 0xE8, TX_EOM at bit 31, TX_STATUS_GOOD at bit 30,
START_XMIT at bit 20, and the divider at bits 11:0. CPU_STATUS bit 6 signals
CEC; the logged bit 1 is not a CEC completion. CNTRL_5 bit 23 selects RX.
A cleared GOOD flag is a NACK only at TX completion, not while waiting.
The offsets, timing field positions, and 400–4700 us thresholds agree with our driver.

[Linux vc4 HDMI](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/gpu/drm/vc4/vc4_hdmi.c)
uses integer `HSM / 40000`, without subtracting one. Thus 163682864 Hz gives
4092 (0xFFC), approximately 40000.7 Hz, and a nominal 25 us tick. The divider
was already correct. Linux reads GOOD at the CEC TX event and maps clear to NACK
(the block makes two attempts). Claiming on a software timeout is our local policy
for this board, not behavior inferred from Linux or proof that the wire is healthy.

[Raspberry Pi firmware documentation](https://www.raspberrypi.com/documentation/computers/legacy_config_txt.html#hdmi_ignore_cec)
says `hdmi_ignore_cec=1` makes firmware ignore CEC and `hdmi_ignore_cec_init=1`
suppresses the initial Active Source message. Both remain in the boot config;
these firmware options do not replace programming the kernel's CEC registers.

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
- Broadcast ACK behavior and claim/receive interoperability on a TV; the empty-bus
  PI-1 bench did exercise one-byte polls and completed broadcasts.
- The one retry of non-poll frames, two retries of transport errors, the 1 s send
  timeout and the 14-character OSD limit (local policies, untested on hardware).
- Where the key mapping puts Root menu (KEY_HOME), Setup menu (KEY_MENU), channel keys and
  colors (policy, not spec).
