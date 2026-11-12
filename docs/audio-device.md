# ABI 1.8 `/dev/audio` (slice 1: ring and null backend)

ABI 1.8 introduced this device and `SBX_IOC_AUDIO_QUERY`; the current kernel
and SDK report 1.11. The HDMI backend below is linked and replaces the null
drain when initialization succeeds. PWM is not implemented. Listening, load
and recovery qualification remain hardware-untested. See [design.md](design.md).

- **Format:** stereo, signed 16-bit little-endian, interleaved, 48000 frames
  a second, fixed. Mono callers duplicate samples. A frame is 4 bytes.
- **Open:** `open("/dev/audio", O_WRONLY)`. Read or read-write is `EACCES`;
  a second open while one is live is `EBUSY` (one writer). The ring and its
  counters start fresh on each open.
- **Write:** whole frames only (`count % 4 != 0` is `EINVAL`). It never
  blocks. It queues what fits and returns that many bytes; nothing fitting is
  `EAGAIN` (wait a little, retry). A zero-byte write is a no-op.
- **Level:** `ioctl(fd, SBX_IOC_AUDIO_QUERY /* 0x20000 */, sbx_audio_info)`:
  rate, capacity (16,384 frames, about 341 ms; `include/audio_ring.h:20`), queued, played, underruns,
  overruns, overrun frames. Output fields must be zero on entry.
- **Underrun:** the backend asks for more than is queued, plays zeros for the
  missing frames and adds one to `underruns`. If foreground refill stalls
  (for example during SD operations), HDMI plays each buffered segment once,
  then silence as those segments recur, within 64 ms. The IRQ retires played
  segments to packed IEC 958 silence; it never consumes the application ring.
  Missed ring drains and their underruns are accounted when the task resumes.
  Ring underruns do not count before the first write of an open, or while closed.

- **Stalled DMA:** if the HDMI DMA completes no segment for 250 ms (`AUDIO_STALL_US`,
  `core/audio_stall.c`), the driver stops it, writes an `audio_stall` line to AUDIO.TXT
  (ring queued and free, DMA segment, completions, `pixel_source`, `pixel_used_hz`, N, CTS),
  and the null drain takes over so `/dev/audio` keeps draining. It does not retry until the
  next HDMI link edge. The write ABI is unchanged: it never blocks; a full ring is a short
  count or `EAGAIN`, so a caller must treat that as "try next frame", never loop on it.
- **Overrun:** the ring never overwrites unread audio. A write that does not
  fit is cut at the last whole frame that fits; each refused write adds one to
  `overruns` and its refused frames to `overrun_frames`. The app sees the short count.
- **Close** (or the program ending, or `sys_reset`): the queue is dropped, not
  played, and the device is free.
- **Blanked screen (ABI 1.12):** while the kernel has the screen blanked, `/dev/audio`
  writes still succeed (full frame count) but are dropped: nothing is queued, so
  AUDIO_QUERY `played` and `queued` stay flat, no stale tail plays at wake, and the
  HDMI backend sends silence without counting underruns. The first write after wake queues
  normally. See [sleep and sound](os-authors.md#sleep-and-sound-abi-112).
- **Ring:** `core/audio_ring.c`, lock-free single producer (the write path)
  and single consumer (the backend), free-running 32-bit frame indices with
  acquire/release ordering. `audio_ring_flush` and `init` move the consumer's
  index and are only for quiet moments (close, open).
- **Null backend:** `core/audio_null.c` drains at exactly 48 frames per 1000
  microseconds from the main loop's `clock_us()`, in 32-bit math (the kernel has
  no 64-bit divide). Tests pass timestamps to `audio_null_tick` directly.

Tests: `tests/test_audio_ring.py` (native: order across wraps, silence,
overrun, pacing, two-thread stress; `AUDIO_SANITIZE=1` adds ASan/UBSan) and
`tests/test_audio_dev.py` (the real dispatcher: open rules, fill, drain pacing,
underrun, overrun, one writer, close while playing, program end, bad records).
The older `core/audio_core.c` (docs/audio-host-core.md) stays unlinked and is
not used by this slice.

**Sample word length (IEC 958, linked HDMI packer).** The ring is 16-bit PCM. In a subframe it
sits in bits 12-27 of the 24-bit sample slot (low 8 bits zero, `core/iec958.c`), and channel status
byte 4 says the same: max length 20 bits, 16 used (`0x02`). The status bits, preambles and N/CTS
values are marked VERIFY in the code and stay outside acceptance until a test on an HDMI TV.

# Slice 3: mixer and PC-speaker voice (kernel-internal)

No new app-facing ABI: `include/mixer.h` and `core/mixer.c` are kernel
internals. The HDMI `TONE` test command uses the mixer; applications still
write PCM to `/dev/audio`, with no public mixer syscall.

- **Mixer:** 8 voices, each with a Q8 volume (256 = full), supporting the full
  uint16 range 0..65535 in tone, tune, and volume updates. `mixer_render`
  sums them in a 32-bit accumulator and saturates to S16, then writes the same
  sample to left and right at 48000 Hz. Tune gain uses a 64-bit product before
  shifting; even eight maximum-volume voices fit the 32-bit sum. Integer only:
  no floats, and no 64-bit divide (a tone's phase step is
  `hz * 89478 + hz * 485 / 1000`, which is
  `hz * 2^32 / 48000`). With nothing active it writes silence.
- **Tone:** a square wave (`mixer_tone`, hz and milliseconds, 0 ms = until
  stopped). Under 20 Hz is refused as inaudible.
- **PC-speaker tune:** `mixer_tune` plays a Game-Maker `.SND` effect: 36
  frequencies in Hz, one per timer tick of 1193180 / 8191 Hz (about 145.67
  ticks a second, 329.5 frames), 0 or under 20 Hz meaning silence, over 20000
  clamped, gain 0.12. These are taken from the engine's TypeScript, not
  invented: `web/packages/engine/src/formats/sound.ts` (36 frequencies, one per
  1/145.6 s tick), `audio/engine.ts` `playSpeaker` (square wave, 20 Hz floor,
  20000 Hz ceiling, gain 0.12, Borland `sound()` programs the PIT) and
  `audio/cmfPlayer.ts` `TICK_RATE = 1193180 / floor(65535 / 8)`. The tick
  boundary is found by cross-multiplication, so it stays exact.
  `code/GM` has only the sound-card header (`SBCVOICE.H`), no speaker timing.
- **Pump:** `mixer_pump_dev(m, max)` renders only what the ring has room for
  (up to `max`) and writes through `audio_dev_write`, so it never overruns.
- **Tests:** `tests/test_audio_mixer.py` (`tests/mixer.c`): zero crossings over
  1 s at five frequencies, duration to the frame, tune ticks (tone, silent
  ticks, second tone, end), two voices summing with clipping, equal-volume sums,
  all 65536 volumes against a widened-integer oracle for tones, tunes, and
  volume updates, the voice limit, idle silence, and a 2 s null-backend run
  with no overrun or underrun.

# Slice 4: HDMI backend (hardware)

`drivers/hdmi_audio.c` replaces the null backend when `hdmi_audio_init` succeeds
(after `fb_start`; this does not guarantee the link clock has settled). Otherwise the null backend stays
and the reason is logged (`AUDIO hdmi: unavailable ...`, also in the `AUDIO` command).
Decisions and firmware observations are saved to `/OS/SAVES/KERNEL/AUDIO.TXT`
and included by `usb_kernel.py logs`. Delayed and hotplug-triggered retries are
bounded; see [audio rules and diagnostics](os-authors.md#audio-rules-and-diagnostics)
for retry/write limits, file format and collection before another boot.
The pure part, `core/hdmi_audio_plan.c` (`include/hdmi_audio.h`), is host-tested by
`tests/test_hdmi_audio.py`.

- **Clocks:** the nominal active pixel clock is queried from firmware (clock ID 9),
  not assumed from framebuffer dimensions. Missing/unsupported clocks keep the null
  backend. 1080p60 is 148.5 MHz; at 48 kHz N = 6144, CTS = 148500 (`hdmi_nct`).
  At 25.2 and 74.25 MHz, N is still 6144 and CTS is 25200 and 74250 respectively.
  The MAI sample divider is the best fraction of HSM clock / 48000 (continued fractions,
  N up to 24 bits, M up to 256). The HSM clock is read from the Clock Manager
  (`CM_HSMCTL` 0x2d6, `CM_HSMDIV` 0x30e0 on the test board: PLLD_PER 500 MHz / 3.0547 =
  163.68 MHz, N = 743393, M = 218).
- **Audio InfoFrame:** type 0x84, version 1, length 10, PB1 = 1 (2 channels, coding type
  from the stream), checksum so the 14 bytes sum to 0, written to packet RAM slot 4 and enabled.
- **MAI and DMA:** the FIFO takes IEC 958 subframes (the packer's output) from a DMA channel
  (1-6, chosen from the firmware's mask) running a circular ring of 8 control blocks, each one 384
  frames (two channel-status blocks, 8 ms), paced by DREQ 17. DMA runs continuously.
  Each segment-complete IRQ replaces the just-played segment with prepacked
  IEC 958 silence and marks it consumed, then increments a monotonic 32-bit
  counter; task progress uses
  unsigned counter differences, so full laps and counter wrap remain visible. IRQs
  must be serviced within 8 ms because the DMA interrupt status is sticky.
  The task refills completed segments from `/dev/audio` (silence when empty;
  underruns counted only after the first write while the device is open).
  The IRQ counts one `starved` segment each time a consumed segment finishes
  playing without a task refill (initial silence is intentional). These counts
  advance during foreground stalls, even with application audio queued. The
  task accounts for all missed ring drains before refilling recent buffers;
  ring underruns count short application queues, separately from starvation.
  Packing uses a separate staging buffer. Publication masks IRQs only for a
  fixed 3 KiB copy and write-buffer drain, and rechecks DMA ownership: neither
  its current nor next segment may be written, leaving at least 8 ms for the
  copy. The IRQ writes only the current control block's predecessor, leaving
  seven segments before reuse. These bounded operations must finish within
  8 ms; their board timing remains untested. No completions for 64 ms adds one
  stall to `starved` until progress resumes. Neither path restarts DMA.
- **Firmware:** `scripts/assemble_boot.py` writes `hdmi_drive=2` (HDMI, not DVI, signaling:
  DVI carries no audio packets). The boot config on the card must carry it.
- **Test hooks (USB):** `TONE <hz> <ms>` (50-4000 Hz, up to 1000 ms; a square tone at about
  10% amplitude through the mixer) and `AUDIO` (backend, segments played, drain rate in
  frames a second, DMA starvation, DMA errors, ring underruns, MAI_CTL).
  `scripts/usb_kernel.py <port> tone 440 1000` / `audio`.
- **Hardware (2026-11-09, second Pi):** the FIFO drains at 47.5-48 kframes/s, no DMA errors, no
  starvation, no ring underruns over seconds of silence. This is drain-counter
  evidence only; audible TV output, concurrent storage/display load, unplug/reconnect
  and OS-fault silence remain hardware-untested.

### Startup correction (2026-11-10)

The failing-board log `MAI_CTL 00003029` alone does **not** establish a stuck
reset: bit 0 is single-shot RESET with undefined readback. The old startup
sent MAI_FMT while MAI was disabled, then enabled MAI only after programming
the core. It also mixed shutdown ERRORF/FLUSH writes into startup, inherited
the firmware's global packet-RAM enable instead of setting it, and ignored
the packet-stop timeout. These are concrete bring-up defects/dependencies;
which one explains a particular board still requires a new hardware trace.

Startup now follows the reference's reset/clock/enable/format ordering:
reset+flush+clear errors, MAI_SMP, MAI enable (2 channels, whole samples,
channel alignment), MAI_FMT, thresholds, core channel configuration and map,
audio sample packet mask, N/CTS, then Audio InfoFrame. Packet RAM's global
enable (bit 16) and the audio slot enable (bit 4) are both set while preserving
other firmware packets. Packet-stop/start and DMA-reset waits are bounded
and fail back to null. The HDMI scheduler must already report HDMI_ACTIVE;
the kernel does not change the firmware's video timing or PHY.

DMA DREQ 17, destination bus address 0x7e808020, 32-bit words, source increment,
fixed destination and burst 2 were already correct. IEC958 packing was also
correct: S16 occupies bits 12–27, validity/user bits are zero, channel status
is bit 30, even parity is bit 31, and B/M/W tags are 8/2/4. New literal-word
tests lock these facts down, including negative PCM and block boundaries.
No GPL driver/conversion code was copied; references are recorded in NOTICE.md.

The old failure message used only the IRQ completion count, not direct FIFO
evidence. Startup now also logs DMA CS/DEBUG/CONBLK and preserves their
pre-reset values for diagnosis. Advancing control blocks without a completion
IRQ gets a distinct IRQ-routing failure instead of being called a DREQ stall.
Null remains active on every failure; DMA errors are classified before reset
can erase them. Boot replay retains the full startup result and diagnostics.

With the generated `hdmi_drive=2`, CEA mode 16 boot config, successful UART
output includes these exact lines (USB replay prefixes each with `BOOT `):

```text
AUDIO hdmi: pixel clock 148500000 Hz (firmware)
AUDIO hdmi: N 6144
AUDIO hdmi: CTS 148500
AUDIO hdmi: MAI_CONFIG 0c000003
AUDIO hdmi: AUDIO_PACKET_CONFIG 21002003
AUDIO hdmi: CRP_CFG 01001800
AUDIO hdmi: DMA destination 7e808020
AUDIO hdmi: DMA draining; HDMI replaces the null backend (silence until /dev/audio or TONE)
```

`RAM_PACKET_CONFIG` must include 0x00010010 plus retained firmware bits.
`DMA channel <n> (32-bit, burst 2, DREQ 17)` identifies the allocated channel.
`DMA completed segments <n> in 30 ms` should normally show 3 (8 ms segments);
counts and status bits are observations, not fixed expected register values.
`MAI_CTL after write` may still show bit 0; do not treat that as failure.
After `INFO`, `TONE 440 1000` should be audible and `AUDIO` should report
`backend=hdmi`, roughly 48000 drain_fps, and no accumulating dma_err.
The backend deliberately transmits silence until PCM or a tone is supplied;
a TV reporting PCM does not itself prove FIFO consumption or audible output.
The corrected startup is host-tested, not yet validated on the failing TV/monitor.

Sources: BCM2835 ARM Peripherals datasheet (DMA); Linux `vc4_hdmi.c`, `vc4_hdmi_regs.h`,
`vc4_regs.h` read for register offsets, bit positions and step order only (GPL, nothing copied);
HDMI 1.4b and CEA-861.

## A foreground or tick stall plays silence, not old audio

The ring is 8 segments of 8 ms and DMA wraps forever, so a segment nobody rewrites is replayed
(a held FM note, garble). `service()` now keeps audio only six segments ahead of DMA (48 ms) and
writes one segment behind the one that just finished, so the segment before DMA's own is always
silence and a wrap lands on it. A tick gap of 48 ms or more (IRQs masked by a long save) also
zeroes every segment except DMA's current one, counts `stall_silence` on the `stat rate` line, and
(poll mode) rebuilds whole laps from the clock, since the DMA position alone hides them. Audio
then resumes in order from the queue (the segments that went by are dropped, as before). What
cannot be fixed here: while IRQs stay masked, DMA still replays what is in the ring (up to 48 ms
of old audio, then silence only once the tick runs). Tests: `tests/hdmi_backend.c` (200 ms stall).

## Periodic health lines in AUDIO.TXT

While HDMI audio runs, the driver adds two `stat` lines every 5 s (`core/audio_stats.c`): `stat rate`
(bytes the DMA really consumed per second against 384000 for 48 kHz stereo, segments, foreground gap
maximum) and `stat flags` (MAI_CTL EMPTY/FULL looks, underflow/overflow/DLATE events, DMA DEBUG error
bits, CS, ring queue). The MAI flags are write-one-to-clear and are cleared after each look. Older
`stat` lines are retired first, so the boot lines survive; saves after the eighth are 30 s apart.
