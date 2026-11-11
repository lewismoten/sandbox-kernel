# USB keyboard mode with an installed host companion

The normal kernel provides the Mac USB link. A separate `USB_ROLE=host` kernel
can stay installed in the idle A/B slot as a **host companion**. On each power-on,
the normal kernel samples the OTG adapter's ID pin. Grounded ID and no enumerated
USB device link select the companion for that boot. The debounce is 300 ms and the boot decision
window is 1.5 s (`USBROLE_HOST_STABLE_US`, `USBROLE_DECISION_WINDOW_US`; the window must clearly outlast the
debounce, which the first hardware test on 2026-11-10 showed). No adapter or a Mac link is decided at once. No confirmed slot,
trial attempt or boot-control record is changed by this handoff. Without the
adapter, the next power-on runs the normal kernel and its Mac link again.

This automatic handoff and the host stack still need hardware qualification.
Connect the adapter **before powering on**. Plugging it into an already running
kernel does not select keyboard mode: ID is sampled only at boot. Unplugging a
keyboard releases its keys, but returning to the Mac link requires powering off,
removing the OTG adapter, and powering on with the normal Mac data cable.

## Install once from a Mac

1. Connect the Pi to the Mac with its normal USB data cable, without the OTG
   adapter. Run these commands from the repository folder. Replace
   `/dev/cu.usbmodemXXXX` with the Pi's actual port (`ls /dev/cu.usbmodem*`).
2. Update the normal device kernel first, then let its 30-second trial confirm:

   ```sh
   make USB_ROLE=device all
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX send build/runtime.ski
   # Reconnect after the reboot; wait at least 30 seconds.
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX info
   ```

   INFO must show `pending=0`, `updates=ready`, and `host_companion=0`.
   If a companion is already installed, `host_companion` may be nonzero.
3. Build and install the keyboard kernel:

   ```sh
   make USB_ROLE=host all
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX install-host build/runtime.ski
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX info
   ```

   Wait for `OK host companion installed`. INFO shows a nonzero
   `host_companion` generation and still shows `pending=0`. Installation does
   not reboot or consume a trial. If the final reply is lost, reconnect and
   inspect INFO before deciding whether to resend.
4. Power off. Disconnect the Mac data cable, attach the OTG adapter and keyboard
   receiver, and power the Pi through its power connector. The normal kernel
   starts first, then hands off to the keyboard kernel. Look for `HOST: keyboard`
   on e-ink and `keyboard ready` on HDMI.
5. To use the Mac link again, power off, remove the adapter, reconnect the normal
   data cable, and power on. Repeat step 4 whenever keyboard mode is wanted;
   the companion remains installed across power cycles.

`USB: OTG: INSTALL KBD` means the keyboard kernel is absent or could not be
validated. Return to the Mac cable and run step 3. The status text uses all 16
available e-ink columns without truncation. UART/USB logs give the longer action.

There are only two kernel slots. A normal kernel update replaces the companion;
install it again after the new device kernel confirms. An interrupted companion
installation can remove keyboard availability, while preserving the confirmed
normal kernel. `make boot` continues to refuse host images as factory defaults.
Use `install-host`, rather than `send`, to keep a keyboard kernel installed.

## Boot and update protections

The image manifest's word 7 identifies device/legacy (0) or host (1). The installer
requires a tagged host image for `PUT2 host-companion`; arbitrary raw code is not
authenticated by this tag. Transport CRC, payload SHA, full image hash and sector
readback checks remain in force. The companion reference is committed only after
readback, separately from pending trial metadata. Before either kind of kernel
installation overwrites the idle slot, an old companion reference is revoked by a
verified metadata commit.

The selector still starts the confirmed device kernel on an ordinary boot. That
kernel verifies the confirmed fallback and companion, rechecks the live USB-link
decision after card reads, deinitializes device USB, and uses the existing
selector handoff record at `0x7000` with boot kind 2. A 72-byte relocated ARM
trampoline at `0x7800` masks interrupts, cleans caches, disables MMU/caches, copies
the verified payload to `0x100000`, and enters the normal runtime startup. The SD
handoff block is refreshed too. This requires no selector replacement on an
existing card and leaves no persistent boot request to survive a power cycle.
The handoff happens before audio or OS startup.

Host confirmation code is excluded at compile time. A host kernel refuses a
confirmed/recovery entry (boot kind 0); it accepts consumed trials (1) or a
matching installed-companion handoff (2). The latter rechecks its slot/generation
against metadata before starting host USB and is never treated as a confirmable
trial. Installed companions remain idle when the adapter is absent. Legacy
one-shot host trials remain a separate mechanism; tagged trials require a
selector that understands the role word, whereas companion handoff does not.
If both control records are lost or the confirmed device image is corrupt,
normal card-reader rescue rules still apply; the companion is not a fallback
for the Mac recovery link. See [M0 A/B update format](m0.md).

## Boot-time ID detection

## Register facts
- GOTGCTL at 0x20980000 (base 0x20980000, GUSBCFG at +0x0C as used in drivers/usb.c).
- GOTGCTL.ConIDSts = bit 16, 1 = B-device, 0 = A-device: Linux `drivers/usb/dwc2/hw.h`,
  `GOTGCTL_CONID_B BIT(16)`. Checked 2026-11-09.
- GUSBCFG.ForceHstMode = bit 29, ForceDevMode = bit 30: same file.
- Forced mode hides the real pin: Linux `dwc2_clear_force_mode()` comment says the
  connector ID status "is affected by the force mode" and allows up to 100 ms of IDDIG
  filter delay after clearing the bits (Linux is a source of facts only; no code used).
- VERIFY: whether the pin reads right before TinyUSB's core reset and PHY setup; the
  Zero W board routes the OTG connector ID pin to the core (believed, not checked).

## The ID sampling window (review R1) and what is guaranteed
Options considered for reading ID without leaving device mode:
- (a) Read ConIDSts, GINTSTS.CurMod or a PHY/GPIO path while ForceDevMode is set. The
  Linux comment above says forcing changes ConIDSts, so ConIDSts is not expected to show the
  pin; CurMod reports the forced mode, not the pin. No GPIO path to the ID pin is known
  (the BCM2835 ID pin is not a GPIO; not checked in the schematic: VERIFY). Rejected as
  unproven.
- (b) Keep ForceDevMode set and clear only ForceHstMode: ForceDevMode itself is what
  changes ConIDSts, so this is not expected to help. Rejected as unproven.
- (c) Chosen: keep the window, make it narrow and checked.

The window (`usb_init`) clears both force bits for 100 ms, once, at boot, then forces
device. During it the code:
- opens the window only if HPRT0.PrtPwr (bit 12) is clear, else it skips it and stays
  device;
- reads HPRT0 (once, to decide) and GOTGCTL (once, after the 100 ms); it writes only GUSBCFG
  (clears both force bits, then forces device). It does not write HPRT0, GHWCFG or any
  host register, never sets ForceHstMode, never starts host init or a soft reset, and the core interrupt handler is not enabled yet
  (`usb_irq` returns until `initialized`);
- forces device straight after (`usbrole_device_gusbcfg`; tested in tests/usbrole.c).

Guarantee for the device-role image and its sampling window, precisely: that code never sets ForceHstMode and never writes HPRT0 (so it never
sets the port power bit or starts host init). The role is not "device for every
microsecond": for about
100 ms at boot the core is in OTG mode, not forced device, so a grounded ID could make the
core report A-device on its own. The kernel does nothing in that state that would drive VBUS:
the port power bit is the software control and it is never set. Whether the core itself
could assert a VBUS-drive pin in that state is VERIFY on hardware. Whether the Pi Zero W's
OTG VBUS is switched by the core or hard-wired to 5 V is also VERIFY: no schematic was
found in this repository; check the Raspberry Pi Zero W schematic before relying on it.
So "device-only" means no host-mode initialization, not that the core is in device mode for
every microsecond.

## Timing (review R2)
`usbrole_step` takes the raw 32-bit `micros()` and compares unsigned microsecond deltas
(`USBROLE_HOST_STABLE_US`, 300,000 us). `micros()` wraps at 2^32 us (71.6 min); dividing it by 1000 first broke the
debounce at the wrap. Tests cover the wrap and the exact 300 ms boundary across it. (TinyUSB's own
`tusb_time_millis_api` in the device role still does `micros()/1000`; the host adapter
now accumulates unsigned microsecond deltas so its retry deadlines survive this wrap.)
- Hot-plug is not implemented: the ID level is sampled once at boot and latched; only the
  "no computer enumerated" half of the decision is live (300 ms debounce, `usbrole_step`).
  Live hot-plug sampling needs a brief un-force window (or the A/B-device change
  interrupt); that disturbs a gadget link, so it is slice 2.

## Host implementation

The build uses the pinned MIT-licensed TinyUSB DWC2 host backend; no Linux or
FreeBSD USB code is copied. One hub and downstream keyboards are supported; live role switching remains future
work. The default device build never initializes host USB itself: only the
verified companion starts that stack.

## Slice 1: descriptor walker and boot-report parser (host-tested, no hardware)

- `core/usbdesc.c`: `usbdesc_find_boot_keyboard` walks a configuration descriptor and returns the
  HID boot keyboard interface (class 3, subclass 1, protocol 1, alternate 0) with its interrupt IN
  endpoint (max packet at least 8), wherever it sits on a composite device. A bad `bLength`, an
  overrun or a truncated buffer returns `USBDESC_E_MALFORMED`; a good descriptor without a keyboard
  returns `USBDESC_E_NOT_FOUND`.
- `core/hidboot.c`: `hidboot_diff` turns each 8-byte report into key down/up changes against the
  previous one (Linux key codes, releases first); a rollover-error report is ignored.
  `hidboot_feed` sends them to `input_frame` in frames of at most `INPUT_FRAME_MAX`.
  `hidboot_release_all` releases held keys on unplug.
- Tests: `tests/hidboot.c`, run by `tests/test_hidboot.py`. The host-role glue
  now calls the report parser. The descriptor walker remains a standalone core;
  TinyUSB performs runtime enumeration. The OS-facing ABI does not change.

## Slice 2: the host-role build (keyboard on the OTG port)

Build: `make USB_ROLE=host all`. The default `USB_ROLE=device` is the Mac link.
A role change relinks everything through `build/usb_role.stamp`. Each runtime
has one fixed USB role; the automatic boot decision selects between images.
There is no CDC link in a host image. It has one hub (`CFG_TUH_HUB 1`, up to four devices behind it) and takes up to
`USBKBD_MAX` (4) boot keyboards at once on any device; a receiver's mouse and consumer interfaces are skipped.

## Boot record, name and the e-ink

Each boot decision attempts to write one line to `/OS/SAVES/KERNEL/BOOT.TXT`: `n<boot counter>,<decision>,k<keyboards found>`
(decisions: `link`, `no-adapter`, `install-kbd`, `keyboard`). The e-ink USB line says `KEYBOARD MODE` (plus the
number of keyboards once found, host role), `NO OTG ADAPTER`, or `OTG: INSTALL KBD`. The host kernel rewrites
the line once the keyboard count has held for 2 s. `INFO` adds `name= boot= lastboot=` (the previous boot's
record) and the `LASTBOOT` command prints them; `usb_kernel.py lastboot` sends it. A Pi's short name
(`usb_kernel.py name PI-2`, 1 to 8 of A-Z 0-9 -, `/OS/NAME.TXT`, ignored if invalid) is drawn in the status screen's
bottom-right corner and shown in INFO.

What is in it: TinyUSB host (`usbh.c`, `hid_host.c`, `hcd_dwc2.c`), slave (PIO) mode, one
hub and up to four downstream devices, HID keyboards. `drivers/usb_host.c` replaces `drivers/usb.c`; `core/usbkbd.c` is the
host-testable glue. TinyUSB sets boot protocol on a boot-subclass HID interface before its
mount callback; the glue arms the interrupt endpoint (at most three immediate submissions total
on each arm; persistent refusal removes that keyboard and releases its keys).
`hidboot_diff` decodes each report; `usbkbd.c` publishes the union of all held keys
through `input_frame`. Unmount releases that keyboard's keys, preserving keys held
on others; `input_remove` runs only after the last keyboard is gone.
`include/tusb_config.h` carries the host settings and a one-line `dwc2_int_set` shim,
because TinyUSB 0.21's `dwc2_bcm.h` has only the device interrupt switch.

The stage messages go to the kernel log (visible in the HDMI recovery console
when no OS owns it, and on UART); the e-ink USB line shows `KEYBOARD MODE`
with the keyboard count when nonzero. `usbkbd_status()` separately reports
`HOST: no device`, `HOST: device, no keyboard` or `HOST: keyboard`. Messages include:

1. `USB host up`: the stack started (it did not hang before this).
2. `device attached VID:PID`: configuration and every class driver completed; the device descriptor is
   recorded earlier in USB.TXT.
3. `keyboard ready`: a boot keyboard interface mounted and polling started. If the device has
   no boot keyboard interface the line is `HID interface skipped (not a boot keyboard)`.
4. `key NAME` for each press (`key Up`, `key Space`, `key LShift`); releases are not shown.
5. `keyboard removed` / `device removed` when it is unplugged.
`keyboard ready` can precede the final device-mounted message. `USB host up`
alone does not distinguish port power/reset from failed enumeration: inspect
USB.TXT descriptor, CTRL/retry and NOT MOUNTED lines. A mounted device without
`keyboard ready` may have no accepted boot keyboard.

Tests: `tests/usbkbd.c` (`make test`) drives the glue with fakes: stage lines, skipped
non-keyboard interfaces, press names, no stuck key on unplug, replug, rollover and short reports.

Unverified on hardware: whether TinyUSB's `hcd_dwc2.c` runs on the Zero's BCM2835 core at all,
the FIFO split, VBUS being powered, and the keyboard dongle being full speed.

## Composite dongles and start-up (review R2, R3)

`CFG_TUH_HID=8` bounds the HID driver table. The host overlay rejects mouse,
consumer-control, report-only and alternate interfaces before allocating a HID
slot or opening endpoints; at most four boot keyboards are opened. Each accepted boot-keyboard (device, HID index) gets its own
slot, up to four total; reports/unmounts affect only the matching slot. The OS
sees the union of their keys. A first report of nine bytes selects leading
report-ID stripping for that keyboard; reports shorter than eight bytes are
ignored. A keyboard that offers no boot protocol is skipped. `input_init()` now runs
before USB or the SD card can deliver a report, so a key held at mount is kept.

## Hardware acceptance for automatic keyboard mode

- Install the new device kernel and companion over the Mac cable. Record INFO's
  confirmed generation, companion generation and `pending=0`.
- Cold boot with the adapter and receiver attached: confirm handoff, `USB host up`,
  attachment VID:PID, `keyboard ready`, key presses, held-key release on unplug,
  and keyboard replug. Inspect the two startup screens and watchdog behavior.
- Repeat cold boots with the adapter several times without reinstalling anything.
  Then boot without it and verify the Mac enumerates, INFO still has the same
  confirmed/companion generations, and the host never confirmed after 30 seconds.
- Test floating ID, grounded ID with an enumerated link where electrically
  appropriate, and a receiver that is not a boot-protocol keyboard. A short link
  loss must not cause a handoff. Verify actual ID sensing, port power/VBUS, DWC2
  reset after device deinit, and SD adoption across the in-memory handoff.
- With no companion, confirm `USB: OTG: INSTALL KBD` fits; with a damaged companion,
  confirm device recovery remains available on the next boot without the adapter.
- Interrupt installation/replacement at slot write and metadata commit boundaries;
  boot without the adapter and confirm the Mac fallback is intact. Perform a
  normal device update, confirm it, then reinstall the companion.

Host tests exercise the role debounce (including timer wrap), link priority,
missing-companion action/width, trial exclusion, real installer/readback/hash
and metadata persistence/revocation. They do not execute ARM cache/MMU transitions,
USB registers or physical power cuts.

## The USB log (USB.TXT)

In the host role the kernel keeps a short text record of the bus and saves it to
`/OS/SAVES/KERNEL/USB.TXT` (an 8192-byte record plus an events line, with 2048 bytes reserved for recent
complete per-second statistics windows. Updates coalesce for 1.5 s, at most
12 saves a boot, only when storage
is ready and no OS is running). A successful save replaces the prior record;
if saving is deferred or fails, the previous boot's file can remain. Per device: address, hub and port
(`otg-port` when it is on the OTG port itself), speed (LS/FS/HS), `split` when the DWC2 driver runs
it through split transactions, VID:PID, device class, EP0 size; per interface: class/subclass/protocol
and endpoints (address, type, max packet, interval); then what the kernel did (`keyboard ACCEPTED`,
`HID skipped: not a boot keyboard`, `HID slots full`, `CONFIG REJECTED: ...`, `mounted`, `removed`). A
device that was read but is not mounted after 4 s gets a `NOT MOUNTED` line (TinyUSB drops a failed
enumeration silently and this build has no TinyUSB debug log). The last line counts attach and remove events. Hub enumeration completion is
tracked explicitly: TinyUSB does not invoke its normal device mount callback for hubs.

The host-only overlay (`patches/tinyusb-0.21-host.patch`, applied to generated copies
under `build/tinyusb-host` by `scripts/usb_host_overlay.py`) keeps the fetched,
hash-pinned upstream sources intact. Both roles also compile the separate
[bounded DWC2 overlay](usb-bounded-waits.md) under `build/tinyusb-bounded`;
the host overlay separately verifies and copies upstream `usbh.c`/`hub.c`/`hid_host.c`
and carries its own task fault guards. The Makefile uses those host files with
the bounded common, OS_NONE and DWC2 sources and the filtered host HID source.
The stack retries failed control transfers three times, after 20/40/80 ms, including
refused controller submissions. A STALL is reported immediately: HID SET_IDLE and
SET_PROTOCOL may legitimately stall, and TinyUSB's HID driver still handles those.
Failed optional string reads skip the remaining strings and continue configuration.
Other unsuccessful enumeration requests now end enumeration rather than treating
STALL as success or leaving a class driver stalled indefinitely.

Hub interrupt bitmaps are accumulated while another device enumerates. Pending bits
are processed one at a time once both enumeration and EP0 are idle, then the hub
interrupt poll is rearmed. This prevents overlapping port-status callbacks from
replacing the shared hub status/callback state. After a failed downstream enumeration,
the stack closes its endpoints and releases its address and keys, waits 200 ms (then
400 ms), and restarts the existing debounce/status/reset sequence, at most twice.
A physical reconnect starts a fresh retry budget. Unplugging the hub clears its queued
changes and recovery state. These delays do not block keyboard polling or USB events.

The host adapter calls `tuh_task_ext(0, false)` and processes at most four USB
events per pass. It also checks a shared 1000 us foreground budget before
controller retries, deferred interrupt submissions and event dispatch; it stops
admitting work at 750 us, reserving 250 us for the next bounded operation. This
is cooperative: a callback already executing and asynchronous IRQ work cannot
be preempted by that check. Lifetime maximum pass time and overruns show whether
the reserve is sufficient on the board. Enumeration and retry deadlines remain asynchronous. The supervisor
excludes measured time inside `usb_task()` from OS readiness and heartbeat
budgets, including USB serviced during a syscall; wall-clock sleep deadlines
and fault restart windows are unchanged. A pass taking at least 2 ms adds one
`USB stall absorbed: slow host pass excluded from OS heartbeat` line per boot.
The four-second incomplete-enumeration watchdog also records
`USB stall absorbed: enumeration pending; kernel loop continues` once per device.
These lines use the existing USB.TXT buffering and save policy (disk writes are
deferred while an OS is running). This protects against false heartbeat faults;
the separate [bounded-wait port](usb-bounded-waits.md) now limits DWC2 polls
and IRQ FIFO drains and disables USB after a terminal timeout. Host tests
do not establish physical latency or interoperability.

## TV slowdown: polling and CPU counters (2026-11-11)

The real TV run reported 114 presents in 19.9 s (about 5.7 FPS) with hub
1A40:0101, keyboard 1997:2433 (boot interrupt 0x81, interval 8; unsupported
0x82, interval 1), and keyboard 1A2C:2124 (boot 0x81, interval 10; another
report-only HID). This is an observation, not a reproduced host benchmark.

Code inspection only partly confirmed the hypothesis. The existing keyboard
mount callback already declined to arm unsupported HID interfaces, so they did
not receive reports. Upstream HID nevertheless opened their endpoints before
that decision. The host overlay now rejects them before opening endpoints or
sending any HID class requests. Indices of accepted keyboards may consequently
change. Advertised boot keyboards that fail to enter boot protocol still get
no report polling. Failed report transfers remove the keyboard and release keys.

DWC2 already used bInterval when retrying periodic NAKs. Successful reports were
immediately rearmed, bypassing that spacing, and pending NAK retries enabled a
125 us SOF software countdown on a high-speed root bus. Both were plausible CPU
costs; neither establishes the cause of the observed frame rate by itself.

All interrupt endpoints, including the hub, now use at least max(bInterval,
8 ms). FS/LS bInterval is in milliseconds; HS uses 125 us * 2^(bInterval-1),
then applies the floor. The host retains a deferred transfer's buffer and busy
state until due. Deadlines advance from the actual submission, without a burst
after late service. Abort, endpoint close and device removal cancel deferred
work. DWC2 also enforces the floor on actual wire polls, including NAK retries;
interrupt NAKs release the channel and wait for a foreground deadline, leaving
SOF IRQs disabled. Complete-split NYET retries remain part of the same poll and
retain their existing three-attempt bound. ISO scheduling is unchanged.
Empty scheduler entries do not mask IRQs or repeatedly read hardware timers.

Once per second, the host appends a bounded statistics window in the reserved
rolling tail of USB.TXT, preserving descriptors and terminal faults. Every five
seconds the same summary and per-endpoint lines go to the kernel status log:

```text
USB win_ms=1000 split=450 passes=1000 task_us=100000
USB irq_us=10000 max_irq=10 max_pass=100 over=0
D1 ep81 submit=125 poll=125 ss=125 cs=125 us=8000
D2 ep81 submit=100 poll=100 ss=100 cs=100 us=10000
D5 ep81 submit=125 poll=125 ss=0 cs=0 us=8000
```

This example is an exact synthetic counter test, not a Pi measurement. Addresses
vary (TinyUSB assigns hubs after the ordinary device range). `win_ms` is the
actual window length, so a delayed sample is not misreported as a one-second
rate. `submit` counts attempts passed to the HCD; `poll` counts actual interrupt
start tokens, including NAK retries; `ss`/`cs` count start/complete split tokens.
The summary's `split` includes split traffic on control endpoints too. Counts
are window deltas, unsigned modulo 2^32. Endpoint counters cover up to 16 distinct
interrupt endpoints; closed rows can be reused by new endpoints. `task_us` is
wall time in the host stack plus its watchdog (including IRQ preemption), while
`irq_us` separately records USB IRQ time. Do not add those overlapping values.
Text formatting/status output is outside those stack timings. `max_irq`,
`max_pass`, and `over` (passes longer than 1000 us) are boot-lifetime values.
The USB.TXT tail retains as many complete recent windows as fit in 2048 bytes;
its maximum size stays 8192 bytes. Disk writes remain deferred while the OS runs;
let Escape return to recovery and allow a save before powering off. Statistics
changes cannot postpone the save deadline indefinitely.

`tests/test_usbhost_poll.py` parses both TV composite configurations through the
real patched host and HID drivers with a configured hub fixture, runs real HID
configuration callbacks and the runtime keyboard/input glue, and returns a
changed Escape report on every accepted poll. With 120 us fake HCD operations
and 1 us fake timer reads, a simulated second gives 125 / 100 keyboard transfers
and 125 hub transfers; unsupported endpoints have zero opens/transfers/class
requests. Minimum keyboard gaps are 8001 / 10011 us. Maximum steady polling
pass is 439 us; maximum including queued event pressure is 882 us, below the
1000 us budget. Tests also cover late service, wrap, abort, unplug, counter
window accounting and full-log retention. A separate register-file test executes
the real DWC2 PIO ACK/NAK paths, counts SS/CS, and checks deadline retries without
SOF IRQs, HS/FS floors, wrap and cancellation. These numbers are deterministic
model costs, not board latency guarantees. Retest FPS/Escape on the TV and compare
`task_us`, `irq_us`, per-endpoint polls, `max_pass` and `over` before attributing
the slowdown or claiming a frame-rate improvement.

`tests/test_usbhost_heartbeat.py` exercises the actual task wrapper and supervisor
with a simulated four-second host call, continued ticks/health reports, readiness
expiry, the remaining heartbeat budget, and unchanged sleep deadlines. The real
stack retry test also holds an accepted control request pending for four seconds
and checks 4,000 returning task passes plus bounded event-burst draining.

A refused hub interrupt-IN submission gets at most three retries after 20/40/80 ms
(four submissions total). Deadlines use elapsed time, not host-task call counts;
even a late task schedules the next delay from the actual refusal, without catch-up
bursts. Each refusal schedules one diagnostic, so repeated calls before its deadline
produce neither submissions nor logs. Exhaustion logs once, closes the hub and its
downstream devices, and marks the hub's upstream port for the same bounded
200/400 ms reset-and-re-enumerate sequence. For a root-connected hub this uses the
OTG root port (`hub0 port0`) and the existing root reset sequence; failed root
enumerations also use this two-retry budget. No interrupt submissions resume on the
closed device. A successful submission clears its poll retry count/deadline; hub
close clears them too. Successful enumeration clears the upstream reset budget.
A root disconnect also cancels its pending reset and clears that budget, so a
physical reconnect can start fresh.

Examples (addresses, port, request and retry count vary):

```text
D1 CTRL 81/06 v=2200 i=0001 len=63 failed retry=1 wait=20ms
D1 CTRL 81/06 v=2200 i=0001 len=63 recovered retry=1
D1 NOT MOUNTED: enumeration failed (see CTRL/retry lines)
hub5 port2 enum reset retry 1/2
hub5 port2 enum recovered 1/2
D5 enumeration complete
hub5 poll refused retry=1 wait=20ms
hub5 poll refused retry=2 wait=40ms
hub5 poll refused retry=3 wait=80ms
hub5 poll exhausted: reset retry=3
hub0 port0 enum reset retry 1/2
hub0 port0 enum reset retry 2/2
hub0 port0 enum exhausted 2/2
```

The CTRL line gives `bmRequestType/bRequest`, `wValue`, `wIndex`, and `wLength`;
`81/06 v=2200` is a report-descriptor read, `00/09` SET_CONFIGURATION,
and `23/03 v=0004 i=0002` a hub PORT_RESET for port 2. Terminal outcomes are
`exhausted` or `stalled`; a recovered control request includes the retry count.
If both port retries fail, expect `hub5 port2 enum exhausted 2/2`; the other ports
continue. The four-second `NOT MOUNTED` watchdog remains for incomplete enumeration,
without claiming every incomplete enumeration was a control failure. An accepted
keyboard interface is not proof that all interfaces finished configuring.
If a refused interrupt submission recovers before exhaustion, expect e.g.
`hub5 poll recovered retry=2` instead of the exhaustion/reset lines. A successful
root retry reports `hub0 port0 enum recovered 1/2` (or `2/2`).

`tests/test_usbhost_retry.py` compiles the actual patched host/hub stack with a fake
HCD and deterministic time: backoff deadlines, refused submits, success/exhaustion,
STALL, optional string refusal, microsecond clock wrap, disconnect cancellation,
deferred multi-port changes, cleanup and a fresh PORT_RESET after enumeration failure.
It also checks 10,000 fixed-time poll refusals, exact backoff boundaries, late-task
non-burst behavior, poll exhaustion through root reset and bounded reset exhaustion,
upstream-port recovery dispatch, downstream cleanup, successful recovery, close
and root-unplug cancellation, and poll
deadline wrap. It requires `make deps` (otherwise skipped).
These tests do not qualify the hub, receiver, bus power or split transactions on hardware.

Read it from a Mac with the normal kernel running (no adapter): `usb_kernel.py <port> usblog`
(`USBLOG <n>` returns the file in numbered pieces, newlines as `|`).

Support facts (checked in the sources, 2026-11-10): TinyUSB 0.21's DWC2 host driver does split
transactions (`hcd_dwc2.c`, `split_en` set for any non-high-speed device when the root port runs at
high speed), so full and low-speed devices behind a high-speed hub are supported. The HID glue accepts
any interface that is a boot keyboard (subclass 1, protocol 1), on any interface number, and ignores
mouse and other interfaces; report IDs on another interface do not matter.

## The fault record (FAULT.TXT)

When the OS program faults (abort, undefined instruction, missed heartbeat, or a nonzero exit), the kernel
attempts to save `/OS/SAVES/KERNEL/FAULT.TXT` before it restarts the program or falls back to DIAG.ELF.
A storage failure logs `Fault record not saved` and recovery continues; an older
record can remain. Line 1 has the
program, the kind, the PC, the fault status and address registers, how long the program ran, its system calls
and presents, and how many times it was started. Each following `>` line is one of the program's last four
log lines (those OS log lines are otherwise only on the serial port). A new fault replaces the file,
so read it after the first crash at the TV. The stored run time truncates elapsed
microseconds to 32 bits before converting to ms, wrapping about every 71.6 min.
`calls` and `starts` are boot-cumulative 32-bit counters; `presents` resets on OS
cleanup. The four recent OS log lines are also kept across launches, so they may
include a previous run. The CLI displays the saved values without correcting
these limits.

With the normal device kernel running and the Mac USB cable attached, use:

```sh
python3 scripts/usb_kernel.py /dev/cu.usbmodem* fault
python3 scripts/usb_kernel.py /dev/cu.usbmodem* fault --elf path/to/GMOS.ELF
python3 scripts/usb_kernel.py /dev/cu.usbmodem* logs --out kernel-logs
```

`fault` fetches the record with GET and prints the program, kind, PC, FSR, address,
run time in ms, calls, presents, start count and last OS log lines. It decodes the
ARM1176 FSR status into words such as alignment, translation, domain or permission
fault (section/page). Zero means no fault status; exit and heartbeat records need
not describe a memory fault. If GET cannot open the record it prints `no fault recorded`.
The protocol's open refusal can also mean inaccessible storage; other transfer or
parse errors remain errors. You can also read the file with a card reader.

`--elf` uses the matching program ELF, preferably with debug information, and the
first tool on PATH among `arm-none-eabi-addr2line`, `llvm-addr2line` and
`llvm-symbolizer` to show PC and LR as `function:file:line`. Without any of these
tools it says symbol resolution is unavailable and still displays the record.
The current kernel does not save LR: the tool labels it unavailable; records that
include `lr 0x...` immediately after PC can resolve both registers. Use the exact
ELF that ran; a different build can give misleading source locations.

`logs` collects `BOOT.TXT`, `USB.TXT`, `CEC.TXT` and `FAULT.TXT` from
`/OS/SAVES/KERNEL/` over one connection, preserving their bytes. Without `--out`
it creates `kernel-logs-YYYYMMDD-HHMMSS` in the current directory. It creates the
output directory as needed, replaces same-named local files on successful reads,
and reports unavailable records while continuing with the others. Existing local
files for unavailable records remain in place; use a fresh directory for each
collection. The records are fetched sequentially, not as one atomic snapshot.
Both commands use GET's chunk CRC checks and require the device-role CDC link;
they do not stop the OS. Host tests in `tests/test_fault_cli.py` cover sample
records, missing files, corrupt transfers and a fake addr2line executable.
