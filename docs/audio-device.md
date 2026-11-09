
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
  rate, capacity (4096 frames, about 85 ms), queued, played, underruns,
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
