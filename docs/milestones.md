# Milestones and acceptance tests

Status, November 10, 2026: M0 board-qualified on the Zero W v1.1 (boot, USB logging,
updates with trial confirmation, automatic fallback, and the e-ink start and status
screens, photographed). Not yet run from M0's list: ten timed cold boots and power
cuts at each transition. M1 in progress ([docs/m1.md](m1.md)). Gates are cumulative;
record board revision, image/firmware hashes, upstream pins, configuration,
timings, logs and failures for each run. Host checks and emulation complement
physical tests but do not establish Pi support. Use spare cards for power cuts.
Exact image sizes and resource budgets must be measured, including vendor files,
selectors, recovery space and both slots; no small-image claim is accepted from
kernel binary size alone.

## M0: Zero W boot, e-ink, USB CDC logging and kernel update

Deliver an original ARMv6 kernel and fixed boot selector, raw SD block access,
V4 panel status, watchdog, CDC-ACM diagnostics, and USB kernel A/B updating.
M0 does not require a user-mode OS or a general FAT filesystem API. The selector
and updater still require raw storage and durable boot-control records.
Vendor firmware stays separate. No Circle source, headers, libraries or binaries
may enter the build. UART is the early boot fallback.

Implementation progress: startup/MMU, UART, timer/IRQ, GPIO/SPI/V4 status, watchdog,
TinyUSB CDC, original EMMC/restricted FAT writer, fixed selector, manifest/hash
validation, alternating control records, trial/fallback/confirmation, stdlib Mac
sender and reproducible boot image are implemented. Host checks run the actual
parser/FAT writer with fake CDC/media. Recorded board runs qualify boot, panel
start/status, CDC logging and verified kernel trials/fallback. The full nine-gate
campaign is incomplete; timed cold boots, injected faults and transition power
cuts remain hardware-untested unless a specific run is recorded. See [M0 implementation and board handoff](m0.md) for exact formats,
commands, pins, measured build sizes and limitations. M0 uses preallocated FAT32
files per the implementation request, overriding the proposed raw partition layout.

Status display behavior (from testing the Circle prototype on the Zero W, 2026-11-06):

- The panel shows "Starting…" as the kernel's first visible act, before USB, SD and timers are up; the full
  status replaces it once initialization finishes (the prototype's first status took about 4 seconds).
- The original M0 cadence was one full refresh at boot, partial refreshes later,
  and no unchanged redraw. ABI 1.9 adds periodic full refreshes for panel care;
  those refresh-care paths are hardware-untested ([eink.md](eink.md)).
- Uptime is compact: seconds below 100 (`42s`), then the two largest units (`49m`, `1h 5m`, `2d 4h`, `7w 1d`,
  `1y 45w`), so the panel redraws at most once a minute after the first 100 seconds.
- The update sender retries the first handshake: the prototype's first send timed out twice before the device
  answered. The kernel answers the handshake before any slow work (panel waveform, storage).

Acceptance tests:

1. Boot a Zero W v1.1 from an MBR/FAT32 image on ten cold starts. Capture
   board/profile, build ID, reset cause and selected slot over UART/CDC. Show a
   deterministic V4 status image. Record time to first log, USB enumeration and
   first completed panel refresh. Validate root boot files and firmware notices.
2. Verify the V4 orientation, pixel bounds, both image planes, full and partial
   refresh, and recovery after reset. Force BUSY stuck high and unplug the panel:
   its five-second timeout must disable panel activity while timers, USB and
   watchdog health continue. Do not hot-plug the HAT to create this test.
3. Enumerate CDC on the Mac with a data cable in the USB socket; test terminal
   open/close, USB reset, disconnect/reconnect and ten reboot reconnects. Logs
   must distinguish enumeration from a connected reader. Bound log queues and
   verify a slow reader cannot stall the kernel. Document temporary USB IDs.
4. Transfer a valid kernel using INFO/PUT2, size+CRC, READY/ACK and VERIFY.
   Read back slot data and metadata, boot the trial, run 30 seconds of kernel
   health checks, and confirm it. A second update must retain the prior confirmed
   kernel. Verify an unchanged known-good OS reference when OS slots are empty
   or not yet enabled, as specified by the M0 format.
5. Reject malformed commands, overflow lengths, wrong profile/type, oversize
   images, wrong CRC/hash, incompatible manifests, truncated payloads and extra
   bytes. Exercise 64-byte, 1,024-byte, exact-multiple, short-final-block and
   maximum-size transfers. Confirm 15-second inactivity cancellation. Compare
   hashes of confirmed slots before and after every rejected update.
6. Install a deliberately nonbooting kernel and a kernel that hangs after entry.
   Hardware watchdog reset must select the confirmed kernel automatically after
   the consumed trial. Test a lost final USB reply and confirm INFO resolves the
   result without guessing or repeating an installation blindly.
7. Cut power during inactive-slot writes, each metadata-copy write, attempt
   consumption, trial execution and confirmation. Repeat each point at least
   ten times across two card models. On restart, either the prior confirmed
   kernel or a completely verified confirmed new kernel must boot; an incomplete
   image must never execute. Test invalidation of either control record and both.
8. Corrupt both runtime slots on spare media. Demonstrate the selector's actual
   recovery behavior and card-reader rescue procedure. Do not claim selector
   CDC recovery unless implemented and tested independently of the runtime.
9. Audit all linked files and their license provenance. Inspect the dependency
   list and release contents for Circle/GPL artifacts. Deliver reproducible image
   layout documentation and a complete notice inventory before passing M0.

Power-cut success applies to the tested failure model and media, not arbitrary
physical card failure. A failure that exposes unsafe metadata ordering blocks
M0; it is not deferred as a general FAT limitation.

## M1: Framebuffer, SD/FAT, protected OS loading and input

Deliver the draft ABI as published SDK headers with fixed class records,
read-only FAT assets, framebuffer presentation, native ELF32 loading in ARM
User mode, input event queues, supervisor recovery, and OS A/B updates. Add the
durable settings/save store before enabling those writes. A basic keyboard or
synthetic event source can establish the input ABI; gamepad qualification is M2.
The current SDK is ABI 1.10. Bounded FAT cache/save writes are implemented
([cache-storage.md](cache-storage.md), [saves-storage.md)); they replace neither
the proposed journal nor the OS A/B/pair-update gates. Physical save/cache
power-cut durability remains hardware-untested. The tests below retain the
long-term journal/pair targets; their presence does not claim implementation.

Acceptance tests:

1. Load two independently built OS programs: a generic diagnostic/demo and the
   first Game-Maker OS program. They must link only against the MIT SDK, render
   distinguishable HDMI patterns, read an SD asset, receive input, and exit or
   replace themselves through the documented calls. No kernel/game-format coupling.
2. Confirm User mode and privileged page protection on physical ARM1176. Attempt
   kernel/MMIO reads and writes, privileged instructions, stack overflow, writes
   to text and execution from writable data. Each execution fault must terminate
   the OS, preserve kernel state, and produce status/log output before restart.
3. Fuzz ELF headers, program tables, entry points, ARM attributes, segment ranges,
   overflows and permissions. Reject ELF64, Thumb entry, unsupported relocations,
   interpreters and over-quota images without losing the recovery console.
4. Test invalid/stale handles, wraparound pointers, unmapped and read-only output
   buffers, excessive counts, unknown calls/ioctls, nonzero reserved fields,
   small query buffers and ABI major mismatch. An older minor-version test app
   must continue running. Verify exact record layouts on both SDK and kernel.
5. Run an infinite user loop, exhaust memory/handles, stop its heartbeat, and
   crash during SD reads, framebuffer presentation and panel refresh. Enforce
   quotas; preempt the loop within two 10 ms ticks; apply the documented restart
   and previous-OS policy. Kernel allocations reserved for recovery must survive.
6. Remove HDMI, omit the panel, and disconnect USB separately. Fault recovery
   must proceed with remaining outputs. Force a driver timeout and a kernel
   progress failure; verify driver isolation where possible and watchdog reboot
   where kernel recovery is necessary.
7. Read FAT32 directories and assets, including empty files and boundary-sized
   reads. Test missing/corrupt media and malformed filesystem metadata with
   bounded failures. Confirm asset and boot FAT sectors remain unchanged.
8. Validate input event timestamps, button/axis values, SYN_REPORT boundaries,
   queue overflow/SYN_DROPPED, current-state resynchronization and device removal.
   Synthetic tests are labeled; at least one real input device must pass.
9. Verify OS-only update success, bad-OS fallback, ABI-incompatible update
   rejection, and paired kernel/OS trial success and failure. Cut power at every
   pair/confirmation transition and verify a compatible confirmed pair survives.
   A healthy OS cannot mask a failed kernel health check.
10. For settings/saves, replay journals after power cuts during append, commit
    and compaction. Reads must return a complete old or new generation, never
    mixed data. Test checksum corruption, full store, quota failure and generation
    conflicts. Do not enable durable writes until these tests pass.

## M2: Sound and gamepads

Implementation status: ABI 1.8 provides stereo S16_LE/48 kHz `/dev/audio` with a
16384-frame ring. HDMI DMA is linked with null fallback; the recorded second-Pi
run shows drain counters only. Listening, sustained load and fault/unplug tests
remain hardware-untested. The kernel-internal mixer serves the USB `TONE` hook.
PWM and gamepad drivers are absent. `make USB_ROLE=host all` links a boot-keyboard
role (hardware-untested, no CDC/hubs, consumed trial only, never confirms); the
default is `USB_ROLE=device`, and `make boot` refuses host runtimes. CEC remains
unlinked host-only code. The acceptance targets below are still gates, not claims
that the original PWM-first plan shipped.

Deliver kernel-managed buffered PCM through PWM first, then investigate HDMI
sound as a distinct gate. The Zero W needs appropriate external filtering/audio
hardware; a GPIO tone is not acceptance of buffered PCM or TV audio. Keep VOC
parsing, CMF/OPL2 synthesis and mixing in the OS program. Add USB host/HID and a
permissively sourced DualSense adapter. USB host play mode uses UART logging
because Zero's controller cannot simultaneously provide CDC gadget mode.

Acceptance tests:

1. Produce a measured PWM tone, then play signed 16-bit mono PCM at a documented
   negotiated rate through the intended external hardware. Check pin allocation
   against the V4 HAT. Measure rate error and confirm clean startup/stop behavior.
2. Sustain PCM for 30 minutes while reading SD assets, rendering HDMI, updating
   e-ink and processing input. Record CPU use, queue depth, underrun count and
   latency. Acceptance target: no unexplained underruns at the selected supported
   rate; forced underrun produces silence and increments a counter.
3. Crash or replace the OS with audio queued; output must become silent and old
   buffers must never be reused by a new OS. Reject invalid formats, lengths and
   over-quota queues without damaging other drivers.
4. On a physical wired DualSense, verify ordinary buttons, D-pad, both sticks and
   triggers against a reference event trace. Test ten unplug/reconnect cycles,
   malformed/truncated reports, hub behavior where supported and simultaneous
   SD/display/audio activity. Haptics, adaptive triggers and controller audio
   remain separate features. Target 99th-percentile input delivery below 20 ms
   under the documented workload; record the actual distribution.
5. Demonstrate development CDC and play host configurations, with clear role
   reporting and no claim of simultaneous operation. Test return to development
   mode and a subsequent kernel update.
6. HDMI audio passes only after a separately audited permissive backend produces
   PCM on real displays, handles supported modes, mute/drain and unplug/reconnect,
   and passes the same sustained-load and OS-fault tests. Until then report
   the actual selected backend (`AUDIO`: HDMI or null); PWM is not implemented
   and capability enumeration is not available.

## M3: Wi-Fi and Bluetooth

Deliver a qualified Zero W CYW43438 SDIO Wi-Fi path, lwIP integration and the
required Bluetooth transport/host profiles. Select and audit driver, security
and firmware dependencies before committing to a stack. Bluetooth Classic HID
support is needed for the intended wireless DualSense; BLE alone does not pass.
Network API records, permissions, signature policy for remote updating, pairing
and key-storage formats require a design extension before exposure to the OS.

Acceptance tests:

1. Record the exact radio firmware/NVRAM hashes, board match, supplier terms and
   shipped notices. Verify the source dependency closure remains permissive.
   Validate radio/regulatory configuration for the test environment.
2. Join a WPA2 network, acquire an address, exchange sustained checked traffic,
   and recover from AP loss, authentication failure and reconnect. Measure packet
   loss, throughput, RAM and CPU while running SD/display/audio/input workloads.
   No network work may starve watchdog, PCM or the supervisor.
3. Pair a DualSense using the required Classic profile, reboot, reconnect using
   stored keys, remove pairing, and recover from controller/radio power loss.
   Verify controls match wired event semantics. Store keys through the durable
   state store; prevent the OS from reading secrets outside its granted scope.
4. Fault or stall radio operations; deadlines and reset paths must preserve the
   recovery interface. Test both Wi-Fi and Bluetooth active together, including
   input latency and audio underruns under traffic.
5. If updates over Wi-Fi are enabled, require the separately specified trusted
   signature and key policy, reject unauthorized/replayed incompatible bundles,
   and repeat the A/B interruption/fallback gates. Network updates are not implied
   by working IP, and SSH is not implied by lwIP.

## Later board qualification

After the Zero W gates pass, port Pi 1 variants, Pi 2, Pi 3/Zero 2, Pi 4 and Pi 5
in that order as practical. Re-run each applicable milestone per board/revision.
Mark unavailable connectors/devices explicitly in the capability inventory.
Run the same generic OS tests against each architecture's native build; a shared
semantic ABI does not make ARM32 and AArch64 executable files interchangeable.
A multi-board SD release requires tested boot selection, separate runtime slots
and recovery on every advertised family. Pi 5/RP1 and Pi 4/5 USB host work are
platform projects, not image-renaming tasks.
