# SPDX-License-Identifier: MIT
"""M1 acceptance 4: real dispatcher, translated user pointers, no boards.

See docs/syscall-acceptance.md for requirements versus robustness cases.
Reuse fixture methods, not Calls' test inheritance (no duplicate discovery).
Never pass a user virtual address to a host memory operation.
"""
import ctypes as C
import hashlib
import struct
import unittest

import test_m1_user as m1

U64_MAX = (1 << 64) - 1


class SyscallEdges(m1.Work):
    REC, BUF = m1.Calls.REC, m1.Calls.BUF
    card = m1.Calls.card
    call = m1.Calls.call
    hdr = staticmethod(m1.Calls.hdr)
    open = m1.Calls.open
    io = m1.Calls.io
    close = m1.Calls.close
    ioctl = m1.Calls.ioctl
    mmap = m1.Calls.mmap

    def setUp(self):
        super().setUp()
        self.card()
        self.addCleanup(self.c.close_disk)
        self.c.us_pool.restype = C.c_void_p
        self.c.take_log()

    def state(self):
        """Snapshot observable state, excluding the intentional sys_calls counter."""
        c = self.c
        return (c.sys_open_handles(), c.us_used(),
                C.c_uint32.in_dll(c, 'sys_presents').value,
                C.c_int64.in_dll(c, 'sys_exit_status').value,
                c.os_running(), c.write_count(),
                hashlib.sha256(C.string_at(c.us_pool(), c.us_used())).digest(),
                hashlib.sha256(C.string_at(self.hw, 2700 * 480)).digest(),
                bytes((C.c_uint32 * 4096).in_dll(c, 'us_table')),
                bytes(((C.c_uint32 * 256) * 16).in_dll(c, 'us_l2')))

    def reject(self, call, record, error, *, va=None, **registers):
        """Install valid host-backed bytes, then prove failure has no side effects."""
        at = self.REC if va is None else va
        if record is not None:
            m1.poke(self.c, at, record)
        before = self.state()
        self.c.take_log()
        self.assertEqual(self.call(call, None, size=registers.pop('size', len(record) if record is not None else 48),
                                   va=at, **registers), -error)
        self.assertEqual(self.state(), before)
        self.assertEqual(self.c.take_log(), b'')

    def records(self):
        _, fd = self.open('/sd/S512.BIN')
        _, log = self.open('/dev/log', 1)
        _, fb = self.open('/dev/fb0', 2)
        _, directory = self.open('/sd')
        m1.poke(self.c, self.BUF + 0x800, b'/dev/log')
        m1.poke(self.c, self.BUF, self.hdr(56) + bytes(40))
        return {
            1: self.hdr(56) + struct.pack('<5Q', self.BUF + 0x800, 8, 1, 0, 0),
            2: self.hdr(48) + struct.pack('<4Q', fd, self.BUF + 0x400, 4, 0),
            3: self.hdr(48) + struct.pack('<4Q', log, self.BUF + 0x400, 4, 0),
            # Exercise seek before close in the successful zero-tail sequence.
            9: self.hdr(48) + struct.pack('<QqQQ', fd, 400, 0, 0),
            4: self.hdr(24) + struct.pack('<Q', fd),
            5: self.hdr(48) + struct.pack('<4Q', fb, 0x10000, self.BUF, 56),
            6: self.hdr(64) + struct.pack('<6Q', fb, 0, 4096, 3, 1, 0),
            16: self.hdr(24) + struct.pack('<q', -7),
            22: self.hdr(40) + struct.pack('<3Q',1,3,0),
            23: self.hdr(32) + struct.pack('<2Q',60,600),
            20: self.hdr(48) + struct.pack('<4Q', directory, self.BUF + 0x400, 512, 0),
        }

    def test_dispatch_envelope_for_every_implemented_call(self):
        for n, good in self.records().items():
            with self.subTest(call=n):
                for major in (0, 2, 0xffffffff):
                    self.reject(n, good, 93, major=major)
                for i in range(4):
                    extra = [0] * 4
                    extra[i] = 0x80000000
                    self.reject(n, good, m1.EINVAL, extra=extra)
                for size in (0, 8, 15, 17, 4097, 0xffffffff):
                    self.reject(n, good, m1.EINVAL, size=size)
                # Header/register sizes agree, but the known record is truncated.
                short = self.hdr(len(good) - 8) + good[16:-8]
                self.reject(n, short, m1.EINVAL)
                for pointer in (0, 0x00100000, 0x20200000, 0xfffffff8,
                                0x7ffffff8, self.REC + 4, m1.IMAGE + 0x3000):
                    self.reject(n, None, m1.EFAULT, va=pointer, size=len(good))

    def test_header_reserved_and_trailing_bytes_for_every_call(self):
        for n, good in self.records().items():
            for offset in (0, 4, 8, 12):
                bad = bytearray(good)
                struct.pack_into('<I', bad, offset, 1)
                with self.subTest(call=n, header_offset=offset):
                    self.reject(n, bytes(bad), m1.EINVAL)
            extended = bytearray(good + bytes(8))
            struct.pack_into('<I', extended, 0, len(extended))
            for at in range(len(good), len(extended)):
                bad = extended.copy()
                bad[at] = 1
                with self.subTest(call=n, tail_byte=at):
                    self.reject(n, bytes(bad), m1.EINVAL)

    def test_zero_extensions_are_accepted_for_every_implemented_call(self):
        for n, good in self.records().items():
            rec = self.hdr(len(good) + 8) + good[16:] + bytes(8)
            with self.subTest(call=n):
                self.assertEqual(self.call(n, rec), 1 if n == 16 else 0)
                self.assertEqual(m1.peek(self.c, self.REC + len(good), 8), bytes(8))

    def test_output_fields_must_be_zero_and_records_writable(self):
        records = self.records()
        for n, output in ((1, 48), (2, 40), (3, 40), (6, 56), (9, 40)):
            good = records[n]
            bad = bytearray(good)
            struct.pack_into('<Q', bad, output, 1 << 63)
            with self.subTest(call=n):
                self.reject(n, bytes(bad), m1.EINVAL)
                self.reject(n, good, m1.EFAULT, va=m1.IMAGE + 0x100)

    def test_unknown_dispatch_does_not_consume_handles_or_records(self):
        records = self.records()
        # Call 9 became real in ABI 1.3; these remain unimplemented.
        for n in (0, 7, 8, 10, 15, 24, 0xffffffff):
            with self.subTest(call=n):
                self.reject(n, records[4], m1.ENOSYS)
        # A valid handle remains usable after all dispatch/header failures.
        fd = struct.unpack_from('<Q', records[4], 16)[0]
        self.assertEqual(self.close(fd), 0)

    def test_maximum_zero_extended_record_and_nonzero_last_byte(self):
        path = self.BUF + 0x800
        m1.poke(self.c, path, b'/dev/log')
        rec = self.hdr(4096) + struct.pack('<5Q', path, 8, 1, 0, 0) + bytes(4096 - 56)
        self.reject(1, rec[:-1] + b'\x01', m1.EINVAL)
        self.assertEqual(self.call(1, rec), 0)
        out = m1.peek(self.c, self.REC, 4096)
        self.assertNotEqual(struct.unpack_from('<Q', out, 48)[0], 0)
        self.assertEqual(out[:48], rec[:48])
        self.assertEqual(out[56:], rec[56:])

    def test_open_pointer_length_and_path_boundary_preserve_state(self):
        for path, size, error in ((self.BUF | (1 << 32), 8, m1.EFAULT),
                                  (U64_MAX - 3, 8, m1.EFAULT),
                                  (0xfffffff8, 16, m1.EFAULT),
                                  (m1.IMAGE + 0x2ffc, 8, m1.EFAULT),
                                  (self.BUF, 256, m1.ENAMETOOLONG),
                                  (self.BUF, U64_MAX, m1.ENAMETOOLONG),
                                  (self.BUF, 0, m1.EINVAL)):
            with self.subTest(path=hex(path), size=size):
                self.reject(1, self.hdr(56) + struct.pack('<5Q', path, size, 1, 0, 0), error)
        # Exact region end and then a split across adjacent readable regions.
        m1.poke(self.c, m1.IMAGE + 0x2ff8, b'/dev/log')
        self.assertEqual(self.open(b'/dev/log', 1, at=m1.IMAGE + 0x2ff8)[0], 0)
        self.assertEqual(self.c.us_map(m1.IMAGE + 0x3000, 4096, m1.R), 0)
        m1.poke(self.c, m1.IMAGE + 0x2ffc, b'/dev')
        m1.poke(self.c, m1.IMAGE + 0x3000, b'/log')
        self.assertEqual(self.open(b'/dev/log', 1, at=m1.IMAGE + 0x2ffc)[0], 0)

    def test_read_range_failures_do_not_advance_file_or_touch_prefix(self):
        _, fd = self.open('/sd/S512.BIN')
        self.assertEqual(self.c.us_map(m1.IMAGE + 0x3000, 4096, m1.R), 0)
        for buf, count, error in ((m1.IMAGE, 4, m1.EFAULT),
                                  (m1.IMAGE + 0x2ff0, 32, m1.EFAULT),
                                  (m1.IMAGE + 0x3ff0, 32, m1.EFAULT),
                                  (self.BUF | (1 << 32), 4, m1.EFAULT),
                                  (0xfffffff8, 16, m1.EFAULT),
                                  (0x7ffffff8, 16, m1.EFAULT),
                                  (self.BUF, (1 << 20) + 1, m1.EINVAL),
                                  (self.BUF, U64_MAX, m1.EINVAL)):
            with self.subTest(buffer=hex(buf), count=count):
                self.reject(2, self.hdr(48) + struct.pack('<4Q', fd, buf, count, 0), error)
        self.assertEqual(self.io(2, fd, self.BUF, 512), (0, 512))
        self.assertEqual(m1.peek(self.c, self.BUF, 512), bytes(range(256)) * 2)
        self.assertEqual(self.io(2, fd, self.BUF, 1), (0, 0))

    def test_read_and_record_can_span_adjacent_writable_regions(self):
        self.assertEqual(self.c.us_map(m1.IMAGE + 0x3000, 4096, m1.R | m1.W), 0)
        _, fd = self.open('/sd/S512.BIN')
        buf = m1.IMAGE + 0x2ff0
        self.assertEqual(self.io(2, fd, buf, 512), (0, 512))
        self.assertEqual(m1.peek(self.c, buf, 16) + m1.peek(self.c, m1.IMAGE + 0x3000, 496),
                         bytes(range(256)) * 2)
        _, log = self.open('/dev/log', 1)
        rec = self.hdr(48) + struct.pack('<4Q', log, self.BUF, 0, 0)
        # poke intentionally only accepts a single translated region: split here.
        va = m1.IMAGE + 0x2ff0
        m1.poke(self.c, va, rec[:16])
        m1.poke(self.c, m1.IMAGE + 0x3000, rec[16:])
        self.assertEqual(self.call(3, None, size=48, va=va), 0)
        self.assertEqual(m1.peek(self.c, va, 16) + m1.peek(self.c, m1.IMAGE + 0x3000, 32), rec)

    def test_record_crossing_readonly_or_unmapped_region_is_atomic(self):
        _, fd = self.open('/sd/S512.BIN')
        good = self.hdr(48) + struct.pack('<4Q', fd, self.BUF, 4, 0)
        at = m1.IMAGE + 0x2ff0
        m1.poke(self.c, at, good[:16])
        self.reject(2, None, m1.EFAULT, va=at, size=48)
        self.assertEqual(self.c.us_map(m1.IMAGE + 0x3000, 4096, m1.R), 0)
        m1.poke(self.c, m1.IMAGE + 0x3000, good[16:])
        self.reject(2, None, m1.EFAULT, va=at, size=48)
        self.assertEqual(self.io(2, fd, self.BUF, 4), (0, 4))
        self.assertEqual(m1.peek(self.c, self.BUF, 4), bytes(range(4)))

    def test_write_boundary_counts_readonly_source_and_log_return(self):
        _, log = self.open('/dev/log', 1)
        for buf, count, error in ((0xfffffff8, 16, m1.EFAULT),
                                  (U64_MAX, 1, m1.EFAULT),
                                  (m1.IMAGE + 0x2ff0, 32, m1.EFAULT),
                                  (self.BUF, (1 << 20) + 1, m1.EINVAL),
                                  (self.BUF, U64_MAX, m1.EINVAL)):
            self.reject(3, self.hdr(48) + struct.pack('<4Q', log, buf, count, 0), error)
        m1.poke(self.c, m1.IMAGE + 0xff8, b'RO text\n')
        self.assertEqual(self.io(3, log, m1.IMAGE + 0xff8, 8), (0, 8))
        self.assertEqual(self.c.take_log(), b'OS: RO text\n')
        # Exactly the documented 1 MiB transfer ceiling, one write, not a flood.
        large = m1.IMAGE + (1 << 20)
        self.assertEqual(self.c.us_map(large, 1 << 20, m1.R | m1.W), 0)
        m1.poke(self.c, large, b'A' * 120)
        self.assertEqual(self.io(3, log, large, 1 << 20), (0, 1 << 20))
        self.assertEqual(self.c.take_log(), b'OS: ' + b'A' * 120 + b'\n')

    def test_invalid_and_stale_handles_across_handle_calls(self):
        records = self.records()
        fd = struct.unpack_from('<Q', records[4], 16)[0]
        self.assertEqual(self.close(fd), 0)
        _, replacement = self.open('/sd/S512.BIN')
        self.assertNotEqual(fd, replacement)
        for bad in (fd, 0, 1, 0x1000010, 1 << 32, U64_MAX):
            for n in (2, 3, 4, 5, 6):
                rec = bytearray(records[n])
                struct.pack_into('<Q', rec, 16, bad)
                want = m1.ENOSYS if n == 6 and bad == U64_MAX else m1.EBADF
                with self.subTest(call=n, fd=bad):
                    self.reject(n, bytes(rec), want)
        self.assertEqual(self.io(2, replacement, self.BUF + 0x400, 4), (0, 4))
        self.assertEqual(m1.peek(self.c, self.BUF + 0x400, 4), bytes(range(4)))

    def test_ioctl_query_envelope_and_outputs_are_atomic(self):
        _, fb = self.open('/dev/fb0', 2)
        q = self.BUF
        good = self.hdr(56) + bytes(40)
        for ptr, size, error in ((q, 0, m1.EINVAL), (q, 48, m1.EINVAL),
                                 (q, 64, m1.EINVAL), (q, 4097, m1.EINVAL),
                                 (q, U64_MAX, m1.EINVAL), (q + 4, 56, m1.EFAULT),
                                 (q | (1 << 32), 56, m1.EFAULT),
                                 (0xfffffff8, 56, m1.EFAULT),
                                 (m1.IMAGE + 0x2ff8, 56, m1.EFAULT),
                                 (m1.IMAGE + 0x100, 56, m1.EFAULT)):
            m1.poke(self.c, q, good)
            m1.poke(self.c, m1.IMAGE + 0x100, good)
            rec = self.hdr(48) + struct.pack('<4Q', fb, 0x10000, ptr, size)
            with self.subTest(pointer=hex(ptr), size=size):
                self.reject(5, rec, error)
        for offset in (0, 4, 8, 12, 16, 24, 32, 40, 48):
            bad = bytearray(good)
            struct.pack_into('<I', bad, offset, 1)
            m1.poke(self.c, q, bytes(bad))
            self.reject(5, self.hdr(48) + struct.pack('<4Q', fb, 0x10000, q, 56), m1.EINVAL)
        m1.poke(self.c, q, good)
        self.assertEqual(self.ioctl(fb, 0x10000, q, 56), 0)
        self.assertEqual(struct.unpack('<4I5Q', m1.peek(self.c, q, 56)),
                         (56, 0, 0, 0, 640, 480, 2560, 1, 640 * 480 * 4))

    def test_unknown_ioctl_and_wrong_device_preserve_state(self):
        _, fb = self.open('/dev/fb0', 2)
        _, log = self.open('/dev/log', 1)
        _, asset = self.open('/sd/S512.BIN')
        for fd, request in ((fb, 0), (fb, U64_MAX), (fb, 0x10004),
                            (log, 0x10000), (asset, 0x10000)):
            self.reject(5, self.hdr(48) + struct.pack('<4Q', fd, request, U64_MAX, 56), m1.ENOTTY)

    def test_mmap_validation_and_repeated_mapping_do_not_allocate(self):
        _, fb = self.open('/dev/fb0', 2)
        need = 640 * 480 * 4
        for offset, length, prot, flags, error in ((0, 0, 3, 1, m1.EINVAL),
                (0, need + 1, 3, 1, m1.EINVAL), (0, U64_MAX, 3, 1, m1.EINVAL),
                (U64_MAX, 4096, 3, 1, m1.EINVAL), (0, 4096, 0, 1, m1.EINVAL),
                (0, 4096, 4, 1, m1.EACCES), (0, 4096, 3, U64_MAX, m1.EINVAL)):
            self.reject(6, self.hdr(64) + struct.pack('<6Q', fb, offset, length, prot, flags, 0), error)
        self.assertEqual(self.mmap(fb, 1), (0, m1.FB_VA))
        used = self.c.us_used()
        self.assertEqual(self.mmap(fb, need), (0, m1.FB_VA))
        self.assertEqual(self.c.us_used(), used)
        self.assertEqual(self.c.us_check(m1.FB_VA, need, m1.R | m1.W), 0)

    def test_present_rejections_preserve_pixels_and_counter(self):
        _, fb = self.open('/dev/fb0', 2)
        q = self.BUF
        rec = self.hdr(48) + struct.pack('<4Q', fb, 0x10001, q, 48)
        m1.poke(self.c, q, self.hdr(48) + struct.pack('<4Q', 0, 0, 1, 1))
        self.reject(5, rec, m1.EINVAL)  # not mapped
        self.assertEqual(self.mmap(fb, 1)[0], 0)
        for rect in ((640, 0, 1, 1), (0, 480, 1, 1), (0, 0, U64_MAX, 1),
                     (0, U64_MAX, 1, 1), (U64_MAX, 0, U64_MAX, 1)):
            m1.poke(self.c, q, self.hdr(48) + struct.pack('<4Q', *rect))
            self.reject(5, rec, m1.EINVAL)
        # Valid bottom-right pixel reaches only that hardware pixel, not padding.
        pixel = struct.pack('<I', 0x123456)
        m1.poke(self.c, m1.FB_VA + (640 * 480 - 1) * 4, pixel)
        before = C.string_at(self.hw, 2700 * 480)
        m1.poke(self.c, q, self.hdr(48) + struct.pack('<4Q', 639, 479, 1, 1))
        self.assertEqual(self.call(5, rec), 0)
        at = 479 * 2700 + 639 * 4
        self.assertEqual(C.string_at(self.hw, 2700 * 480), before[:at] + pixel + before[at + 4:])
        self.assertEqual(C.c_uint32.in_dll(self.c, 'sys_presents').value, 1)

    def test_input_only_records_may_be_readonly_and_exit_is_signed(self):
        _, fd = self.open('/dev/log', 1)
        rec = self.hdr(24) + struct.pack('<Q', fd)
        at = m1.IMAGE + 0x100
        m1.poke(self.c, at, rec)
        self.assertEqual(self.call(4, None, size=24, va=at), 0)
        self.assertEqual(m1.peek(self.c, at, 24), rec)
        _, fb = self.open('/dev/fb0', 2)
        m1.poke(self.c, self.BUF, self.hdr(56) + bytes(40))
        rec = self.hdr(48) + struct.pack('<4Q', fb, 0x10000, self.BUF, 56)
        m1.poke(self.c, at, rec)
        self.assertEqual(self.call(5, None, size=48, va=at), 0)
        self.assertEqual(m1.peek(self.c, at, 48), rec)
        for status in (0, -1, -(1 << 63), (1 << 63) - 1):
            rec = self.hdr(24) + struct.pack('<q', status)
            m1.poke(self.c, at, rec)
            self.assertEqual(self.call(16, None, size=24, va=at), 1)
            self.assertEqual(C.c_int64.in_dll(self.c, 'sys_exit_status').value, status)
            self.assertEqual(m1.peek(self.c, at, 24), rec)


if __name__ == '__main__':
    unittest.main()
