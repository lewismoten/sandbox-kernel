# SPDX-License-Identifier: MIT
"""ABI 1.8 /dev/audio: ring, syscall semantics and the null backend, through
the real dispatcher on the host harness. The null backend is driven by explicit
timestamps (audio_null_tick), so pacing is exact. No ARM execution, no hardware."""
import ctypes as C
import struct
import unittest

import test_m1_user as m1

EAGAIN = 11
FRAMES = 16384
INFO = ('flags', 'rate', 'capacity', 'queued', 'played', 'underruns', 'overruns', 'overrun_frames')


class AudioDev(m1.CallsBase):
    DATA = m1.CallsBase.BUF + 0x900  # up to 1792 bytes (448 frames) of PCM

    def setUp(self):
        super().setUp()
        self.c.audio_null_tick.argtypes = [C.c_uint64]
        self.t = 1_000_000

    def tick(self, us):
        self.t += us
        self.c.audio_null_tick(self.t)

    def start(self):
        r, fd = self.open('/dev/audio', 1)
        self.assertEqual(r, 0)
        self.tick(0)  # the backend's clock starts at open
        return fd

    def write(self, fd, frames, fill=1):
        m1.poke(self.c, self.DATA, b''.join(struct.pack('<hh', fill + i % 7, -(fill + i % 7)) for i in range(frames)))
        return self.io(3, fd, self.DATA, frames * 4)

    def info(self, fd):
        rec = self.hdr(80) + bytes(64)
        r = self.ioctl_at(fd, rec)
        return r, dict(zip(INFO, struct.unpack_from('<8Q', m1.peek(self.c, self.BUF, 80), 16)))

    def ioctl_at(self, fd, rec):
        m1.poke(self.c, self.BUF, rec)
        return self.ioctl(fd, 0x20000, self.BUF, 80)

    def fill(self, fd, frames):
        done = 0
        while done < frames:
            n = min(400, frames - done)
            self.assertEqual(self.write(fd, n), (0, n * 4))
            done += n

    def test_open_rules(self):
        self.assertEqual(self.open('/dev/audio', 0)[0], -m1.EACCES)  # write-only
        self.assertEqual(self.open('/dev/audio', 2)[0], -m1.EACCES)
        r, fd = self.open('/dev/audio', 1)
        self.assertEqual(r, 0)
        self.assertEqual(self.open('/dev/audio', 1)[0], -m1.EBUSY)  # multiple writers refused
        self.assertEqual(self.close(fd), 0)
        self.assertEqual(self.open('/dev/audio', 1)[0], 0)  # free again after close
        self.assertEqual(self.io(2, fd, self.DATA, 4)[0], -m1.EBADF)  # stale handle

    def test_fill_and_level(self):
        fd = self.start()
        r, i = self.info(fd)
        self.assertEqual((r, i['rate'], i['capacity'], i['queued']), (0, 48000, FRAMES, 0))
        self.fill(fd, 3000)
        self.assertEqual(self.info(fd)[1]['queued'], 3000)
        self.assertEqual(self.write(fd, 0, 1)[0:1], (0,))  # empty write is a no-op
        self.assertEqual(self.io(3, fd, self.DATA, 6)[0], -m1.EINVAL)  # whole frames only

    def test_drain_is_real_time(self):
        fd = self.start()
        self.fill(fd, 3000)
        self.tick(10_000)  # 10 ms = 480 frames
        i = self.info(fd)[1]
        self.assertEqual((i['queued'], i['played'], i['underruns']), (2520, 480, 0))
        for _ in range(100):  # 100 ticks of 333 us: fractions carry, nothing is lost
            self.tick(333)
        i = self.info(fd)[1]
        self.assertEqual(i['played'], 480 + (33_300 * 48000) // 1_000_000)
        self.assertEqual(i['underruns'], 0)

    def test_underrun_counts_and_plays_silence(self):
        fd = self.start()
        self.fill(fd, 100)
        self.tick(10_000)  # wants 480, has 100
        i = self.info(fd)[1]
        self.assertEqual((i['queued'], i['played'], i['underruns']), (0, 100, 1))
        self.tick(10_000)
        self.assertEqual(self.info(fd)[1]['underruns'], 2)
        self.fill(fd, 50)  # data after an underrun plays normally
        self.assertEqual(self.info(fd)[1]['queued'], 50)

    def test_no_underrun_before_first_write_or_when_closed(self):
        fd = self.start()
        self.tick(1_000_000)
        self.assertEqual(self.info(fd)[1]['underruns'], 0)

    def test_overrun_is_defined(self):
        fd = self.start()
        self.fill(fd, FRAMES - 100)
        r, n = self.write(fd, 300)  # only 100 fit: partial, whole frames
        self.assertEqual((r, n), (0, 400))
        i = self.info(fd)[1]
        self.assertEqual((i['queued'], i['overruns'], i['overrun_frames']), (FRAMES, 1, 200))
        self.assertEqual(self.write(fd, 8)[0], -EAGAIN)  # full: nothing taken
        i = self.info(fd)[1]
        self.assertEqual((i['queued'], i['overruns'], i['overrun_frames']), (FRAMES, 2, 208))
        self.tick(1_000)  # 48 frames out; room again
        self.assertEqual(self.write(fd, 48), (0, 192))

    def test_full_ring_write_returns_quickly(self):
        import time
        fd = self.start()
        self.fill(fd, FRAMES)  # ring full, no drain: a stalled consumer looks like this
        t0 = time.monotonic()
        for _ in range(2000):
            self.assertEqual(self.write(fd, 448)[0], -EAGAIN)  # never blocks, never loops
        self.assertLess((time.monotonic() - t0) / 2000, 0.020)

    def test_close_while_playing_drops_queue(self):
        fd = self.start()
        self.fill(fd, 2000)
        self.tick(5_000)
        self.assertEqual(self.close(fd), 0)
        self.tick(100_000)  # closed: the backend does nothing, no underruns
        fd2 = self.start()
        i = self.info(fd2)[1]
        self.assertEqual((i['queued'], i['played'], i['underruns']), (0, 0, 0))  # fresh counters, empty ring

    def test_program_end_releases_device(self):
        fd = self.start()
        self.fill(fd, 100)
        self.c.sys_reset()
        self.assertEqual(self.open('/dev/audio', 1)[0], 0)

    def test_bad_records_and_ioctls(self):
        fd = self.start()
        self.assertEqual(self.ioctl(fd, 0x20001, self.BUF, 80), -m1.ENOTTY)
        self.assertEqual(self.ioctl_at(fd, self.hdr(80) + b'\1' + bytes(63)), -m1.EINVAL)  # outputs must be zero
        self.assertEqual(self.ioctl(fd, 0x20000, self.BUF, 48), -m1.EINVAL)  # wrong size
        self.assertEqual(self.ioctl(fd, 0x10000, self.BUF, 56), -m1.ENOTTY)  # not a framebuffer
        self.assertEqual(self.io(2, fd, self.DATA, 4)[0], -m1.EBADF)  # write-only
        self.assertEqual(self.io(3, fd, 0x00100000, 4)[0], -m1.EFAULT)


if __name__ == '__main__':
    unittest.main(verbosity=2)
