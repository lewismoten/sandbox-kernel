# SPDX-License-Identifier: MIT
"""Bounded M1 resource campaigns: real core, isolated host-library per test.

No host OOM, process-limit or timing tests. USB queue tests compile the real
USB glue separately because the shared core harness replaces its logging sink.
"""
import ctypes as C
import hashlib
import pathlib
import struct
import subprocess
import tempfile
import unittest

import test_m1_user as host

PAGE = 4096
MIB = 1 << 20
CYCLES = 4


class Exhaustion(host.Work):
    # Reuse only fixture/helpers, not Calls' inherited test methods.
    REC, BUF = host.Calls.REC, host.Calls.BUF
    card = host.Calls.card
    call = host.Calls.call
    hdr = staticmethod(host.Calls.hdr)
    open = host.Calls.open
    io = host.Calls.io
    close = host.Calls.close
    mmap = host.Calls.mmap

    def setUp(self):
        super().setUp()
        self.card()
        self.addCleanup(self.c.close_disk)

    def memory_state(self):
        """Accounted pages, all descriptors/regions and every live pool byte."""
        c = self.c
        c.us_pool.restype = C.c_void_p
        used = c.us_used()
        return (used, C.c_uint.in_dll(c, 'us_count').value,
                bytes((C.c_uint32 * 4096).in_dll(c, 'us_table')),
                bytes(((C.c_uint32 * 256) * 16).in_dll(c, 'us_l2')),
                bytes((C.c_uint32 * 32).in_dll(c, 'us_regions')),
                hashlib.sha256(C.string_at(c.us_pool(), used)).digest())

    def assert_map_failure(self, va, size, reason):
        before = self.memory_state()
        self.assertEqual(self.c.us_map(va, size, host.R | host.W), -12)
        self.assertEqual(host.text(self.c, 'us_fail'), reason)
        self.assertEqual(self.memory_state(), before)

    def test_program_handles_close_reuse_and_reset(self):
        for cycle in range(CYCLES):
            fds = []
            for count in range(16):
                rc, fd = self.open('/dev/log', 1)
                self.assertEqual(rc, 0)
                self.assertNotIn(fd, fds)
                fds.append(fd)
                self.assertEqual(self.c.sys_open_handles(), count + 1)
            memory = self.memory_state()
            saved_record = host.peek(self.c, self.REC, 56)
            for _ in range(3):
                self.assertEqual(self.open('/dev/log', 1), (-host.EMFILE, 0))
                expected = self.hdr(56) + struct.pack('<5Q', self.BUF + 0x800, 8, 1, 0, 0)
                self.assertEqual(host.peek(self.c, self.REC, 56), expected)
                self.assertEqual(self.c.sys_open_handles(), 16)
                host.poke(self.c, self.REC, saved_record)
                self.assertEqual(self.memory_state(), memory)
                # All already-issued handles still work, including the last slot.
                for fd in fds:
                    self.assertEqual(self.io(3, fd, self.BUF, 0), (0, 0))
            old = fds.pop(cycle)
            self.assertEqual(self.close(old), 0)
            self.assertEqual(self.c.sys_open_handles(), 15)
            rc, new = self.open('/dev/log', 1)
            self.assertEqual(rc, 0)
            self.assertNotEqual(new, old)
            self.assertEqual(self.close(old), -host.EBADF)
            self.assertEqual(self.c.sys_open_handles(), 16)
            for fd in fds + [new]:
                self.assertEqual(self.close(fd), 0)
            self.assertEqual(self.c.sys_open_handles(), 0)
            # Reset must invalidate live handles too, not just explicitly closed ones.
            rc, old = self.open('/dev/log', 1)
            self.assertEqual(rc, 0)
            self.c.sys_reset()
            self.assertEqual(self.close(old), -host.EBADF)
            self.assertEqual(self.c.sys_open_handles(), 0)

    def test_asset_subquota_failure_does_not_leak_program_slots(self):
        for cycle in range(CYCLES):
            fds = [self.open('/sd/S512.BIN') for _ in range(8)]
            self.assertTrue(all(rc == 0 for rc, _ in fds))
            for _ in range(24):  # more failures than the whole program table
                self.assertEqual(self.open('/sd/S512.BIN'), (-host.EMFILE, 0))
                self.assertEqual(self.c.sys_open_handles(), 8)
            for _, fd in fds:
                self.assertEqual(self.io(2, fd, self.BUF, 1), (0, 1))
                self.assertEqual(host.peek(self.c, self.BUF, 1), b'\0')
            old = fds.pop(cycle)[1]
            self.assertEqual(self.close(old), 0)
            rc, new = self.open('/sd/S512.BIN')
            self.assertEqual(rc, 0)
            self.assertNotEqual(new, old)
            self.assertEqual(self.io(2, old, self.BUF, 1), (-host.EBADF, 0))
            # The eight slots not used by assets remain usable by devices.
            for _ in range(8):
                self.assertEqual(self.open('/dev/log', 1)[0], 0)
            self.assertEqual(self.c.sys_open_handles(), 16)
            self.assertEqual(self.open('/dev/log', 1), (-host.EMFILE, 0))
            self.c.sys_reset()  # must release both layers of handles
            self.assertEqual(self.c.sys_open_handles(), 0)
            for fd in [fd for _, fd in fds] + [new]:
                self.assertEqual(self.close(fd), -host.EBADF)

    def test_pool_exact_limit_atomic_failure_and_reset_reuse(self):
        kernel = bytes(self.c.kernel_table().contents)
        for _ in range(CYCLES):
            self.assertEqual(self.c.us_reset(self.c.kernel_table()), 0)
            # One mapping uses exactly all 4096 pages and all sixteen L2 tables.
            self.assertEqual(self.c.us_map(host.IMAGE, host.POOL_BYTES,
                                           host.R | host.W), 0)
            self.assertEqual(self.c.us_used(), host.POOL_BYTES)
            host.poke(self.c, host.IMAGE, b'pool-first')
            host.poke(self.c, host.IMAGE + host.POOL_BYTES - 16, b'pool-last')
            for _ in range(3):
                self.assert_map_failure(host.IMAGE + host.POOL_BYTES, PAGE, 'USER POOL')
            self.assertEqual(bytes(self.c.kernel_table().contents), kernel)
            self.assertEqual(self.c.us_check(0x100000, PAGE, host.R), -host.EFAULT)
            self.assertEqual(self.c.us_reset(self.c.kernel_table()), 0)
            self.assertEqual(self.c.us_used(), 0)
            self.assertEqual(C.c_uint.in_dll(self.c, 'us_count').value, 0)
            self.assertEqual(self.c.us_check(host.IMAGE, PAGE, host.R), -host.EFAULT)
            self.assertEqual(self.c.us_map(host.IMAGE, host.POOL_BYTES,
                                           host.R | host.W), 0)
            # All reused pages are zero, not just the first/last sampled bytes.
            self.c.us_pool.restype = C.c_void_p
            self.assertEqual(C.string_at(self.c.us_pool(), host.POOL_BYTES),
                             bytes(host.POOL_BYTES))

    def test_region_limit_atomic_failure_and_reset_reuse(self):
        for _ in range(CYCLES):
            self.assertEqual(self.c.us_reset(self.c.kernel_table()), 0)
            for i in range(8):
                self.assertEqual(self.c.us_map(host.IMAGE + i * PAGE, PAGE,
                                               host.R | host.W), 0)
            self.assertEqual(self.c.us_used(), 8 * PAGE)
            for _ in range(3):
                self.assert_map_failure(host.IMAGE + 8 * PAGE, PAGE, 'USER MAP REGIONS')
            # No individual unmap API exists: reset is the supported release.
            self.assertEqual(self.c.us_reset(self.c.kernel_table()), 0)
            self.assertEqual(self.c.us_map(host.IMAGE + 8 * PAGE, PAGE, host.R), 0)

    def test_l2_limit_with_pool_space_left_is_atomic(self):
        for _ in range(CYCLES):
            self.assertEqual(self.c.us_reset(self.c.kernel_table()), 0)
            # Fifteen sections with only fourteen MiB + one page of pool use.
            self.assertEqual(self.c.us_map(host.IMAGE + MIB - PAGE,
                                           14 * MIB + PAGE, host.R | host.W), 0)
            self.assertEqual(self.c.us_map(host.IMAGE + 16 * MIB, PAGE,
                                           host.R | host.W), 0)
            self.assertLess(self.c.us_used() + PAGE, host.POOL_BYTES)
            for _ in range(3):
                self.assert_map_failure(host.IMAGE + 17 * MIB, PAGE, 'USER L2')
            # A new page in an existing section still succeeds: no leaked slots.
            self.assertEqual(self.c.us_map(host.IMAGE + 16 * MIB + PAGE, PAGE,
                                           host.R | host.W), 0)

    def test_fb_mmap_exhaustion_preserves_record_and_recovers_on_restart(self):
        for _ in range(CYCLES):
            rc, fb = self.open('/dev/fb0', 2)
            self.assertEqual(rc, 0)
            # The live program already has text/data/stack/startup mappings.
            # Fill the remaining regions, leaving syscall records accessible.
            i = 0
            while C.c_uint.in_dll(self.c, 'us_count').value < 8:
                self.assertLess(i, 8)
                self.assertEqual(self.c.us_map(host.IMAGE + MIB + i * PAGE,
                                               PAGE, host.R | host.W), 0)
                i += 1
            before = self.memory_state()
            saved_record = host.peek(self.c, self.REC, 64)
            record = self.hdr(64) + struct.pack('<6Q', fb, 0, 640 * 480 * 4, 3, 1, 0)
            for _ in range(3):
                self.assertEqual(self.mmap(fb, 640 * 480 * 4), (-12, 0))
                self.assertEqual(host.peek(self.c, self.REC, 64), record)
                # Exclude only the syscall record we intentionally wrote.
                host.poke(self.c, self.REC, saved_record)
                self.assertEqual(self.memory_state(), before)
            self.c.os_handle(5, 0, 0);host.drain(self.c)
            if self.c.os_running():  # first fault may auto-restart once
                self.c.os_handle(5, 0, 0);host.drain(self.c)
            self.assertFalse(self.c.os_running())
            self.assertEqual(self.c.sys_open_handles(), 0)
            self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'), 0)
            self.assertEqual(self.close(fb), -host.EBADF)
            rc, fb = self.open('/dev/fb0', 2)
            self.assertEqual(rc, 0)
            self.assertEqual(self.mmap(fb, 640 * 480 * 4), (0, host.FB_VA))
            self.c.os_handle(5, 0, 0);host.drain(self.c)
            if self.c.os_running():
                self.c.os_handle(5, 0, 0);host.drain(self.c)
            self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'), 0)

    def test_fault_recovery_after_simultaneous_pool_and_handle_exhaustion(self):
        kernel = bytes(self.c.kernel_table().contents)
        for _ in range(CYCLES):
            assets = [self.open('/sd/S512.BIN') for _ in range(8)]
            devices = [self.open('/dev/log', 1) for _ in range(8)]
            self.assertTrue(all(rc == 0 for rc, _ in assets + devices))
            self.assertEqual(self.open('/dev/log', 1), (-host.EMFILE, 0))
            self.assertEqual(self.c.us_reset(self.c.kernel_table()), 0)
            self.assertEqual(self.c.us_map(host.IMAGE, host.POOL_BYTES,
                                           host.R | host.W), 0)
            self.assert_map_failure(host.FB_VA, PAGE, 'USER POOL')
            # Inject a user fault, not a real ARM instruction. Recovery must
            # close both handle layers before opening/reloading the OS file.
            self.c.take_log()
            self.c.os_handle(5, 0, 0);host.drain(self.c)
            if self.c.os_running():
                self.c.os_handle(5, 0, 0);host.drain(self.c)
            self.assertFalse(self.c.os_running())
            self.assertEqual(self.c.sys_open_handles(), 0)
            self.assertIn(b'kernel recovery console kept', self.c.take_log())
            self.assertEqual(bytes(self.c.kernel_table().contents), kernel)
            self.assertEqual(C.string_at(self.hw, 4), b'\xff\xff\xff\0')
            self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'), 0)
            self.assertEqual(self.c.us_used(), MIB + 4 * PAGE)
            for _, fd in assets + devices:
                self.assertEqual(self.close(fd), -host.EBADF)
            rc, asset = self.open('/sd/S512.BIN')
            self.assertEqual(rc, 0)
            self.assertEqual(self.io(2, asset, self.BUF, 1), (0, 1))
            self.assertEqual(self.close(asset), 0)

    def test_log_syscalls_remain_bounded_and_leave_other_resources_usable(self):
        rc, fd = self.open('/dev/log', 1)
        self.assertEqual(rc, 0)
        payload = b'L' * 120 + b'not-logged' * 12
        host.poke(self.c, self.BUF, payload)
        self.c.take_log()
        used = self.c.us_used()
        for _ in range(CYCLES):
            for _ in range(512):
                self.assertEqual(self.io(3, fd, self.BUF, len(payload)), (0, len(payload)))
            # This sink is the core harness, NOT the USB queue. Each OS line
            # is still capped at SYS_LOG_LINE and the dispatch stays usable.
            lines = self.c.take_log().splitlines()
            self.assertTrue(lines)
            self.assertTrue(all(line == b'OS: ' + b'L' * 120 for line in lines))
            self.assertEqual(self.c.us_used(), used)
            self.assertEqual(self.c.sys_open_handles(), 1)
            rc, asset = self.open('/sd/S512.BIN')
            self.assertEqual(rc, 0)
            self.assertEqual(self.io(2, asset, self.BUF, 1), (0, 1))
            self.assertEqual(self.close(asset), 0)
            host.poke(self.c, self.BUF, payload)
        self.assertEqual(self.close(fd), 0)


class UsbLogExhaustion(unittest.TestCase):
    def test_real_queue_slow_reader_and_recovery_reply_reserve(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = pathlib.Path(directory) / 'usb-exhaustion'
            subprocess.run(['cc', '-std=c11', '-O1', '-DHOST_TEST', '-Wall',
                            '-Wextra', '-Werror', '-Wno-misleading-indentation',
                            '-I' + str(host.ROOT / 'tests/exhaustion_stubs'),
                            '-I' + str(host.ROOT / 'include'),
                            str(host.ROOT / 'tests/exhaustion_usb.c'), '-o', str(exe)],
                           check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
