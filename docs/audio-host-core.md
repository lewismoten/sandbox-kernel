# Unlinked PCM host core

`core/audio_core.c` and `include/audio_core.h` are a fixed-memory qualification
component, deliberately absent from `KERNEL` in the Makefile. They implement no
syscall, SDK record, device path, availability flag, hardware backend or IRQ.
This component does not change the ABI. The current runtime/SDK is ABI 1.11;
[the linked audio device and HDMI backend](audio-device.md) use `audio_ring.c`
and `audio_null.c`, independently of this qualification core.

## Internal contract

A kernel-owned `struct audio_core` contains 4096 stereo S16_LE frames (16384
bytes). Initialize once on a new object; reset preserves generation history.
One nonzero generation token owns writes; second open is BUSY, old tokens STALE.
Generation exhaustion fails closed rather than recycling tokens. Closing/faulting
clears samples and removes ownership; STOP clears samples but retains ownership.
Reset clears counters and ownership without recycling generations. Open starts
new per-open counters. Fault increments a saturating error counter; drain counts
actual copied source frames and counts each short nonzero drain as one underrun.
All counters saturate at UINT64_MAX; they never wrap into smaller values.

`audio_core_write` takes a **trusted, mapped, readable span**, checked offset and
byte count. It copies immediately; it never retains a source pointer. A nonzero
write must start on a 4-byte boundary and contain whole frames, at most 1920.
Entire capacity or range/alignment/owner failure leaves the entire state unchanged.
A valid owner's zero-byte write is a no-op even with NULL/irrelevant offset.
Insufficient capacity returns AGAIN, not a partial write. Range subtraction and
address-addition checks precede dereference. `audio_core_drain` accepts at most
1920 frames into private output scratch, copies queued L/R bytes and writes zero
for missing frames, never stale data. Zero drain does not count an underrun.

This trusted-span seam is **not a user address/permission validator**. Future
integration must perform the real checked dispatcher range/permission/lifetime
validation, then serialize the whole validation/copy/publication operation.
All calls, including drain/stop/reset, require serialized callers. No IRQ-safe
publication, concurrent producer, DMA coherence or mapping revocation is claimed.
Private spans/output must not overlap the object. No address is returned to apps.

## Reproduce

`python3 tests/test_audio_core.py` compiles/runs the actual production component.
`AUDIO_SANITIZE=1 python3 tests/test_audio_core.py` adds ASan/UBSan. The normal
suite discovers the same test. Assertions cover copied bytes, wrap/capacity,
zero/alignment/oversize/range/address-overflow rejection, stale/second owners,
stop/close/fault/reset, silence, saturating counters and generation exhaustion.
A separate linear FIFO oracle checks 20000 deterministic state transitions.

## Remaining integration gates

The approved private HDMI design remains controlling. H1 requires actual Zero W
firmware/config/EDID/sink/clock evidence and approved compatible hardware-driver
provenance. Circle is GPL reference-only: none of its implementation is used here.
HDMI DREQ17 is not DMA channel17; a firmware-safe DMA channel must be claimed and
released, with ARM1176 alignment/bus-alias/cache/barrier handling and bounded waits.
This component supplies none of IEC958 framing, infoframes, DMA descriptors or
IRQ dispatch. The separate linked backend implements those paths, and ABI 1.8
allocated `/dev/audio`; full USB/timer coexistence under load is hardware-untested.

Synchronous SD readiness/transfer/programming waits can exceed this 85.333 ms
software ring. Host draining in a test does not prove producer schedulability.
Actual card-starvation windows, safe storage service architecture if needed,
physical underruns/tick drops, TV listening and recovery remain Mac-owned gates.
No USB F/B1 code is integrated by this component. No installable audio candidate
or hardware acceptance is implied by host/ARM/QEMU tests or benchmark results.

Original core and tests: Lewis Moten, MIT; AI implementation and qualification:
Hermes GPT-6.1 Sol via openai-codex. No external driver/synth algorithm was adapted.
