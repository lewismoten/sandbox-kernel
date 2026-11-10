
ABI minor 1.8 adds this device (and `SBX_IOC_AUDIO_QUERY`); the saves slice keeps 1.7 (`docs/saves-storage.md`). Both headers say minor 8.
# ABI 1.8 `/dev/audio` (slice 1: ring and null backend)

No sound hardware yet. This is the app-facing device and the ring behind it;
a PWM or HDMI backend later replaces `audio_null_tick` with a DMA refill and
keeps everything above it. Design: docs/design.md (`/dev/audio`).

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
  missing frames (never stale data) and adds one to `underruns`. Nothing counts
  before the first write of an open, or while closed.
- **Overrun:** the ring never overwrites unread audio. A write that does not
  fit is cut at the last whole frame that fits; each refused write adds one to
  `overruns` and its refused frames to `overrun_frames`. The app sees the short count.
- **Close** (or the program ending, or `sys_reset`): the queue is dropped, not
  played, and the device is free.
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

**Sample word length (IEC 958, not yet on any wire).** The ring is 16-bit PCM. In a subframe it
sits in bits 12-27 of the 24-bit sample slot (low 8 bits zero, `core/iec958.c`), and channel status
byte 4 says the same: max length 20 bits, 16 used (`0x02`). The status bits, preambles and N/CTS
values are marked VERIFY in the code and stay outside acceptance until a test on an HDMI TV.

# Slice 3: mixer and PC-speaker voice (kernel-internal)

No new app-facing ABI: `include/mixer.h` and `core/mixer.c` are kernel
internals, so `docs/os-authors.md` is unchanged. Nothing calls the mixer yet; a
later slice feeds `/dev/audio` from it.

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
(after `fb_start`, so the firmware's HDMI is up). Otherwise the null backend stays
and the reason is logged (`AUDIO hdmi: unavailable ...`, also in the `AUDIO` command).
The pure part, `core/hdmi_audio_plan.c` (`include/hdmi_audio.h`), is host-tested by
`tests/test_hdmi_audio.py`.

- **Clocks:** 1080p60 is 148.5 MHz; at 48 kHz N = 6144, CTS = 148500 (`hdmi_nct`).
  The MAI sample divider is the best fraction of HSM clock / 48000 (continued fractions,
  N up to 24 bits, M up to 256). The HSM clock is read from the Clock Manager
  (`CM_HSMCTL` 0x2d6, `CM_HSMDIV` 0x30e0 on the test board: PLLD_PER 500 MHz / 3.0547 =
  163.68 MHz, N = 743393, M = 218).
- **Audio InfoFrame:** type 0x84, version 1, length 10, PB1 = 1 (2 channels, coding type
  from the stream), checksum so the 14 bytes sum to 0, written to packet RAM slot 4 and enabled.
- **MAI and DMA:** the FIFO takes IEC 958 subframes (the packer's output) from a DMA channel
  (1-6, chosen from the firmware's mask) running a circular ring of 8 control blocks, each one 384
  frames (two channel-status blocks, 8 ms), paced by DREQ 17. DMA runs continuously.
  Each segment-complete IRQ increments a monotonic 32-bit counter; task progress uses
  unsigned counter differences, so full laps and counter wrap remain visible. IRQs
  must be serviced within 8 ms because the DMA interrupt status is sticky.
  The task refills completed segments from `/dev/audio` (silence when empty;
  underruns counted only after the first write while the device is open).
  A delay of eight or more completions increments starvation and accounts for missed
  drains before refilling the latest seven safe buffers. No completions for 64 ms
  counts one stall until progress resumes. Neither path restarts DMA.
- **Firmware:** `scripts/assemble_boot.py` writes `hdmi_drive=2` (HDMI, not DVI, signaling:
  DVI carries no audio packets). The boot config on the card must carry it.
- **Test hooks (USB):** `TONE <hz> <ms>` (50-4000 Hz, up to 1000 ms; a square tone at about
  10% amplitude through the mixer) and `AUDIO` (backend, segments played, drain rate in
  frames a second, DMA starvation, DMA errors, ring underruns, MAI_CTL).
  `scripts/usb_kernel.py <port> tone 440 1000` / `audio`.
- **Hardware (2026-11-09, second Pi):** the FIFO drains at 47.5-48 kframes/s, no DMA errors, no
  starvation, no ring underruns over seconds of silence.

Sources: BCM2835 ARM Peripherals datasheet (DMA); Linux `vc4_hdmi.c`, `vc4_hdmi_regs.h`,
`vc4_regs.h` read for register offsets, bit positions and step order only (GPL, nothing copied);
HDMI 1.4b and CEA-861.
