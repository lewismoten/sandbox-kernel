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


    def log(self):
        """The SPI command stream since the last reset: (command, first data bytes, data byte count)."""
        out = []
        for i in range(self.c.panel_probe_log_count()):
            e = (C.c_ubyte*13)()
            self.c.panel_probe_log_entry(i, e)
            n = e[9] | e[10] << 8 | e[11] << 16
            out.append((e[0], bytes(e[1:9])[:min(n, 8)], n))
        return out

    def ram(self):
        a, b = C.create_string_buffer(4000), C.create_string_buffer(4000)
        self.c.panel_probe_ram(a, b)
        return a.raw, b.raw

    def glass(self):
        plane = C.create_string_buffer(4000)
        self.assertTrue(self.c.panel_snapshot(plane))
        return plane.raw

    def test_boot_full_writes_both_planes_whole(self):
        self.boot()
        log = self.log()
        refresh = [e for e in log if e[0] == 0x22][0]
        self.assertEqual(refresh[1][:1], b'\xf7')
        for plane in (0x24, 0x26):
            self.assertEqual([e[2] for e in log if e[0] == plane][0], 4000)

    def test_partial_writes_only_the_changed_window_and_syncs_old_ram(self):
        self.boot()
        page = bytearray(eink.PAGE)
        self.jump(self.c.clock_us()+3_000_000)
        self.assertEqual(self.c.eink_page(bytes(page), len(page), C.c_uint64(self.c.clock_us())), 0)
        self.pump()
        self.jump(self.c.clock_us()+3_000_000)
        page[1000] = 0x55  # one byte of the page: a small rectangle of the panel
        self.c.panel_probe_log_reset()
        self.assertEqual(self.c.eink_page(bytes(page), len(page), C.c_uint64(self.c.clock_us())), 0)
        self.pump()
        log = self.log()
        self.assertEqual([e[0] for e in log], [0x3c, 0x18, 0x01, 0x11, 0x44, 0x45, 0x4e, 0x4f, 0x24, 0x22, 0x20,
                                               0x44, 0x45, 0x4e, 0x4f, 0x26, 0x10])
        self.assertEqual(log[0][1][:1], b'\x80')  # border waveform for partial updates
        self.assertEqual(log[1][1][:1], b'\x80')  # internal temperature sensor: a reset forgets it
        self.assertEqual(log[9][1][:1], b'\xff')  # partial refresh mode
        self.assertEqual(log[16][1][:1], b'\x01')  # deep sleep mode 1 keeps RAM
        x0, x1 = log[4][1][:2]
        y0, y1 = log[5][1][0] | log[5][1][1] << 8, log[5][1][2] | log[5][1][3] << 8
        n = (x1-x0+1)*(y1-y0+1)
        self.assertLess(n, 100)
        self.assertEqual((log[8][2], log[15][2]), (n, n))  # new and old planes get the same small window
        self.assertEqual(log[11][1][:2], bytes([x0, x1]))  # the old plane is synced over the same rectangle
        self.assertEqual(log[6][1][:1], bytes([x0]))
        a, b = self.ram()
        self.assertEqual(a, self.glass())  # new plane holds what was sent
        self.assertEqual(b, self.glass())  # old plane equals what is on the glass after every partial

    def test_seconds_tick_is_a_small_window(self):
        self.boot()
        self.pump(8000)
        self.c.panel_probe_log_reset()
        self.pump(2500)
        log = self.log()
        windows = [e for e in log if e[0] == 0x24]
        self.assertGreaterEqual(len(windows), 1)
        self.assertTrue(all(e[2] < 1000 for e in windows), windows)
        a, b = self.ram()
        self.assertEqual(a, self.glass())
        self.assertEqual(b, self.glass())

    def test_power_saving_freezes_uptime_with_sleep(self):
        self.c.power_timeouts.restype = C.c_int
        self.c.power_input.restype = None
        self.boot()
        self.pump(8000)
        self.assertEqual(self.c.power_timeouts(C.c_uint64(20), C.c_uint64(0), C.c_uint64(self.c.clock_us())), 0)
        self.pump(18000)  # awake: the seconds still tick
        before = self.glass()
        self.pump(4000)  # the 20 s idle mark passes: one redraw showing SLEEP
        entered = self.glass()
        self.assertNotEqual(entered, before)
        self.c.render_status_sleep(True)
        drawn = []
        for sec in range(20, 60):  # the frozen uptime is one of these: the plane must be the SLEEP screen
            plane = C.create_string_buffer(4000)
            self.c.render_status(plane, b'PANEL TEST', b'reader', b'none', C.c_uint32(sec))
            drawn.append(plane.raw)
        self.c.render_status_sleep(False)
        self.assertIn(entered, drawn)
        n1 = self.c.panel_probe_partial_count()
        self.pump(5000)
        self.assertEqual(self.c.panel_probe_partial_count(), n1)  # no redraw while asleep
        self.assertEqual(self.glass(), entered)
        self.c.power_input(C.c_uint64(self.c.clock_us()))  # input wakes it
        self.pump(3000)
        self.assertGreater(self.c.panel_probe_partial_count(), n1)  # woke: redrawn with the live uptime
        self.assertNotEqual(self.glass(), entered)
        self.pump(3000)
        n2 = self.c.panel_probe_partial_count()
        self.pump(3000)
        self.assertGreater(self.c.panel_probe_partial_count(), n2)  # seconds tick again

    def test_capture_tracks_trigger_not_scheduled_or_failed_plane(self):
        self.c.panel_snapshot.restype = C.c_bool
        plane = C.create_string_buffer(4000)
        self.assertFalse(self.c.panel_snapshot(plane))
        self.pump(127)  # the first successful full-refresh trigger
        self.assertTrue(self.c.panel_snapshot(plane))
        sent = plane.raw
        expected = C.create_string_buffer(4000)
        self.c.render_starting(expected)
        self.assertEqual(sent, expected.raw)
        self.pump(173)
        self.assertTrue(self.c.panel_snapshot(plane))
        sent = plane.raw
        page = bytes(eink.PAGE)
        self.assertEqual(self.c.eink_page(page, len(page), C.c_uint64(self.c.clock_us())), 0)
        self.c.panel_probe_fault(2)
        self.pump(300)
        self.assertTrue(self.c.panel_probe_disabled())
        self.assertTrue(self.c.panel_snapshot(plane))
        self.assertEqual(plane.raw, sent)  # failed/new desired plane never replaces sent data

    def test_second_refresh_after_deep_sleep_waits_for_wake(self):
        self.c.panel_probe_busy_commands.restype = C.c_uint
        self.c.panel_probe_wakes.restype = C.c_uint
        self.boot()
        page = bytearray(eink.PAGE)
        for i in range(3):  # three updates in a row, each waking the sleeping controller
            self.jump(self.c.clock_us()+3_000_000)
            page[0] = 0x80 if i & 1 else 0x40
            self.assertEqual(self.c.eink_page(bytes(page), len(page), C.c_uint64(self.c.clock_us())), 0)
            self.pump()
            self.assertGreaterEqual(self.c.panel_probe_partial_count(), i+1)
        self.assertGreaterEqual(self.c.panel_probe_wakes(), 3)
        self.assertEqual(self.c.panel_probe_busy_commands(), 0)  # no write reached the controller during sleep or wake BUSY
        self.assertFalse(self.c.panel_probe_disabled())

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
