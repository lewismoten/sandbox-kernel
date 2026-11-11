# Idle power saving (ABI 1.11)

The kernel parks an idle foreground loop with ARM1176 CP15 WFI. This is the
`mcr p15,0,Rd,c7,c0,4` operation; the instruction-set WFI mnemonic is a NOP on
this core. A runnable OS or incremental address-space cleanup keeps running.
No foreground task or IRQ calls the idle wait recursively.

Timer compare 1 stays at 10 ms for existing user slices, tick health and watchdog
feeding. Compare 3 is an additional one-shot idle wake, no later than 1 ms or the
OS's absolute `sys_wake_us` deadline, whichever is earlier. The ARM IRQ mask is
saved while arming/waiting and restored afterward; pending hardware interrupts
wake WFI even while exception delivery is masked. A deadline that passes during
arming skips WFI. This avoids both an interrupt-before-wait race and missing a
very short timer compare. The timer is the independent BCM2835 1 MHz system
clock; lowering the ARM frequency does not change deadlines. Compare 3 does not
increment scheduler `ticks`.

| Work source | Wake/service path |
| --- | --- |
| USB device CDC | Existing DWC2 IRQ 9; deferred events and buffered output serviced next pass, at most 1 ms additional idle wait |
| USB host HID/hub | Existing DWC2 IRQ 9 including host channel/SOF handling; software retry/enumeration deadlines also polled within 1 ms |
| SD | Synchronous bounded controller polling, with `service_io`; no outstanding SD request when main parks |
| HDMI audio DMA | Existing IRQs 17–22, one per 384-frame/8 ms segment; foreground refill immediately after waking |
| Null audio | Elapsed-time accounting each pass, within 1 ms during idle |
| CEC | Register polling each pass, within 1 ms; no new CEC IRQ routing required |
| OS sleep, supervisor heartbeat | Deadline-shortened timer plus existing supervisor checks; runnable apps are never parked |
| Panel, autostart, watchdog, protocol timeouts/log flush | Polled within 1 ms while idle; their existing elapsed-time guards remain |
| Firmware power reply | Polled within 1 ms; no mailbox interrupt required |

These are bounds on the added idle wait, not guarantees that existing synchronous
SD, framebuffer mode changes or panel operations take less than 1 ms. Audio IRQs
remain enabled during those operations as before. No board latency measurements
have been made for this change.

`core/power.c` owns the host-testable policy. Defaults are 60 seconds before
slowdown and 600 seconds before blanking. Input changes and held-key repeats
count even with no open input reader. CEC recognizes unmapped button presses as
activity, plus directed Image View On and broadcast Active Source. Acquiring a
hold inhibits the selected transition and requests restoration if already idle.
Releasing the last hold permits an already expired timeout immediately. An OS
cleanup clears all holds and timeout overrides and restarts the default policy.
Token IDs never repeat across launches, and exhaustion fails closed.

`drivers/power.c` reads the running ARM rate after `cpu_speed()` and the firmware
minimum (falling back to 700 MHz, capped at the running rate). Only the ARM clock
(id 3) changes. Runtime SET_CLOCK_RATE and framebuffer blank requests use a
separate strongly ordered message at physical 0x80100, next to the existing
256-byte mailbox area at 0x80000. Runtime requests never spin waiting for firmware;
all mailbox readers route replies to the correct owner. A reply late by 100 ms
logs once and keeps the GPU-owned buffer reserved until completion, allowing a
late slowdown reply to be followed by restoration after intervening input.
Invalid replies log and retry after five seconds. A firmware request that never
replies prevents further power changes, but does not stall the kernel loop.
Clock changes log the returned rate in Hz; screen changes and failures also log.

Blanking uses firmware Blank Screen, retaining the framebuffer allocation and
contents; firmware mode changes cause blanking to be reasserted. It then queues CEC Standby directed to TV address 0. Wake unblanks and queues
Image View On and Active Source using the claimed playback address. Queue pressure
or an address claim in progress retains the latest intent for a later pass.
An absent/disabled CEC link cannot guarantee TV standby or wake. These operations
have no new raw hardware app capability. ABI records, errors and movie examples
are in [os-authors.md](os-authors.md#idle-power-abi-111).

Host validation uses the production policy and dispatcher, translated user memory,
OS exit/fault/STOP cleanup, mocked firmware replies, and fake CEC registers.
The idle wait model produces **1,000 timer wakeups per simulated second** without
other interrupt traffic, and shortens a wait to a deadline one microsecond away.
The 10 ms scheduler IRQ and USB SOF/audio IRQs can add or coincide with those
wakeups on a board. This model does not execute ARM WFI or measure power use.

Validation on the macOS host, 2026-11-10: `make test` passed 475 tests in
244.064 seconds, with one existing independent desktop FAT writer test skipped
because mtools/mkfs.fat are unavailable. `make all` and `make USB_ROLE=host all`
both passed with Arm GNU 15.3.rel2 and the pinned TinyUSB 0.21.0 sources.
The final build output is device-role. ARM disassembly confirms the CP15 drain,
WFI and IRQ-mask save/restore sequence. No AddressSanitizer or DYLD overrides were
used. No physical board, current-draw or wake-latency result is claimed.

Physical acceptance remains open. On a Zero W v1.1, record firmware version,
config.txt, both kernel image hashes, TV model/CEC configuration, USB topology,
logs, timings and USB-meter readings:

1. Measure steady current with an idle OS that uses `sleep_until`, after 60 seconds
   at the lower clock, and after 10 minutes with HDMI blank/TV standby. Compare
   against the parent kernel using the same power supply, cable and peripherals.
2. Temporarily set short timeouts to check clock logs, blank/standby, every USB
   keyboard and remote key, unmapped CEC keys, directed Image View On and broadcast
   Active Source. Verify framebuffer contents and HDMI audio resume after wake.
3. Play stereo PCM while exercising USB device transfers and a host keyboard/hub;
   inspect `AUDIO` underrun/starvation/DMA counters and keyboard/poll latency before
   and after slowdown. Repeat across blank/wake and a framebuffer mode change.
4. Play video for 2–3 hours with both hold bits, continue heartbeats and absolute
   sleeps, and verify no slowdown or standby. Test separate/nested holds and
   timeout disable; exit, fault, restart and USB STOP must restore defaults and
   invalidate old tokens. Cross the 32-bit microsecond wrap (about 71 minutes).
5. Exercise unplug/replug, USB suspend/resume, SD reads/writes, CEC claim failure,
   missing HDMI/TV, and an unresponsive firmware property service. Recovery I/O and
   watchdog feeding must continue. Do not infer board support from host/build success.

Primary references (facts only, no imported source): Broadcom *BCM2835 ARM
Peripherals*, 06 February 2012, chapters 7 and 12 (IRQ and timer compare); Arm
[*ARM1176JZF-S TRM*](https://documentation-service.arm.com/static/5e8e294efd977155116a6ca3), DDI 0301H r0p7, section 3.2.22 (p. 3-85) and section 10.2.2 (p. 10-3), c7 Wait For
Interrupt and Standby; Raspberry Pi firmware [Mailbox property interface](https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface),
revision 2023-12-07, consulted 2026-11-10 (ARM clock id, Get clock rate,
Get min clock rate, Set clock rate and Blank screen). Existing CEC/audio sources
and provenance remain as recorded in NOTICE.md and docs/cec.md.
