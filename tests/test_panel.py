# SPDX-License-Identifier: MIT
"""E3/E4 production panel state machine, core ledger and syscall device seam.
SPI/GPIO and elapsed time are simulated; no physical panel timing claim.
"""
import ctypes as C
import unittest
import hostlib
import test_eink as eink
from test_m1_user import SupervisorBase, poke


class Panel(SupervisorBase):
    BUFP = SupervisorBase.BUF
    dev = eink.Eink.dev
    ready = eink.Eink.ready
    write_page = eink.Eink.write_page
    title = eink.Eink.title
    clear = eink.Eink.clear

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        hostlib.build(cls.work/'core.dylib', panel_probe=True)

    def setUp(self):
        super().setUp()
        self.c.clock_us.restype = C.c_uint64
        self.c.eink_available.restype = C.c_bool
        self.c.panel_probe_disabled.restype = C.c_bool
        for name in ('boot_trigger', 'last_full_trigger', 'last_trigger'):
            getattr(self.c, 'panel_probe_'+name).restype = C.c_uint64
        self.c.panel_start()

    def pump(self, milliseconds=300):
        for _ in range(milliseconds):
            self.c.advance(1000)
            now = self.c.clock_us()
            self.c.panel_task(b'reader', now//1_000_000)

    def jump(self, target):
        while self.c.clock_us() < target:
            self.c.advance(min(target-self.c.clock_us(), 1_000_000_000))
            self.c.clock_us()  # extend the real 32-bit timer without missing wraps

    def boot(self):
        self.pump()
        self.assertEqual(self.c.panel_probe_full_count(), 1)
        self.assertEqual(self.c.panel_probe_boot_trigger(), 127_000)

    def test_boot_full_command_spacing(self):
        self.boot()
        page = bytearray(eink.PAGE)
        self.assertEqual(self.c.eink_page(bytes(page), len(page), C.c_uint64(self.c.clock_us())), 0)
        self.pump()
        for i in range(1, 81):
            self.jump(3_000_000+i*2_100_000)
            page[0] = 0x80 if i & 1 else 0
            self.assertEqual(self.c.eink_page(bytes(page), len(page), C.c_uint64(self.c.clock_us())), 0)
            self.pump()
        self.assertGreaterEqual(self.c.panel_probe_partial_count(), 50)
        self.jump(180_000_000)
        page[1] = 0x80
        self.assertEqual(self.c.eink_page(bytes(page), len(page), C.c_uint64(self.c.clock_us())), 0)
        self.pump(66)
        # Review witness: 180,066,000 - 127,000 = 179,939,000 (too short).
        self.assertEqual(self.c.clock_us(), 180_066_000)
        self.assertEqual(self.c.panel_probe_full_count(), 1)
        self.assertEqual(self.c.panel_probe_last_full_trigger(), 127_000)
        self.pump(234)
        self.jump(181_100_000)
        page[1] = 0
        self.assertEqual(self.c.eink_page(bytes(page), len(page), C.c_uint64(self.c.clock_us())), 0)
        self.pump()
        self.assertEqual(self.c.panel_probe_full_count(), 2)
        last = self.c.panel_probe_last_full_trigger()
        self.assertGreaterEqual(last-self.c.panel_probe_boot_trigger(), 180_000_000)
        self.c.eink_full_due.restype = C.c_bool
        self.assertFalse(self.c.eink_full_due(C.c_uint64(last+179_999_999)))

    def test_static_page_daily_full_command(self):
        self.boot()
        page = bytes(eink.PAGE)
        self.assertEqual(self.c.eink_page(page, len(page), C.c_uint64(self.c.clock_us())), 0)
        self.pump()
        before = self.c.panel_probe_partial_count()
        deadline = 127_000+23*3600*1_000_000
        self.jump(deadline-1)
        self.c.panel_task(b'reader', 0)
        self.assertEqual(self.c.panel_probe_full_count(), 1)
        self.jump(deadline)
        self.pump()
        self.assertEqual(self.c.panel_probe_full_count(), 2)
        self.assertEqual(self.c.panel_probe_partial_count(), before)
        self.assertLess(self.c.panel_probe_last_full_trigger(), 127_000+24*3600*1_000_000)

    def check_fault(self, kind, message):
        fd = self.ready()
        poke(self.c, self.BUFP, self.hdr(80)+bytes(64))
        self.assertEqual(self.ioctl(fd, eink.Q, self.BUFP, 80), 0)
        self.c.panel_probe_fault(kind)
        self.pump(6000)
        self.assertTrue(self.c.panel_probe_disabled())
        self.assertFalse(self.c.eink_available())  # review witness was disabled=1/available=1
        self.assertEqual(C.c_char_p.in_dll(self.c, 'last_error').value, message)
        poke(self.c, self.BUFP, self.hdr(80)+bytes(64))
        self.assertEqual(self.ioctl(fd, eink.Q, self.BUFP, 80), -eink.ENODEV)
        self.assertEqual(self.write_page(fd, bytes(eink.PAGE)), -eink.ENODEV)
        self.assertEqual(self.title(fd, 'SCOOT'), -eink.ENODEV)
        self.assertEqual(self.clear(fd), -eink.ENODEV)
        self.assertEqual(self.close(fd), 0)
        self.assertEqual(self.dev()[0], -eink.ENODEV)

    def test_spi_timeout_disables_provider(self):
        self.check_fault(2, b'SPI timeout')

    def test_busy_timeout_disables_provider(self):
        self.check_fault(1, b'Panel BUSY timeout')


if __name__ == '__main__':
    unittest.main()
