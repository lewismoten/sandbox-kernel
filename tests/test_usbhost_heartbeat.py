# SPDX-License-Identifier: MIT
"""Actual USB task wrapper and OS supervisor, deterministic stalled host work."""
import ctypes as C
import pathlib
import re
import subprocess
import sys
import tempfile

import hostlib
import test_autostart
import test_m1_user

ROOT = pathlib.Path(__file__).resolve().parents[1]


class USBHeartbeat(test_m1_user.Work):
    card = test_autostart.Autostart.card

    def setUp(self):
        self.c = test_m1_user.lib(self.work)
        self.c.autostart_run.argtypes = [C.c_char_p]
        self.c.clock_us.restype = C.c_uint64
        self.c.host_delay.argtypes = [C.c_uint32]
        self.c.usblog_text.restype = C.c_char_p
        self.c.usblog_text.argtypes = [C.POINTER(C.c_uint32)]

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        task = re.search(r'void usb_task\(void\) \{.*?\n\}',
                         (ROOT / 'drivers/usb_host.c').read_text(), re.S).group(0)
        helper = cls.work / 'usb-task.c'
        helper.write_text('''#include "kernel.h"
#include "clock.h"
#include "os.h"
#include "usblog.h"
static bool initialized=true;
static uint32_t delay;
static bool stalled_register;
void advance(uint32_t us);
static uint32_t usb_poll_micros(void) {advance(1);return micros();}
void idle_wait(uint32_t us) {advance(us);}
#define micros usb_poll_micros
#include "drivers/usb_wait.c"
#undef micros
void host_stuck_register(void) {stalled_register=true;}

unsigned usb_wait_requested;
void advance(uint32_t us);
void host_delay(uint32_t us) {delay=us;}
static void tuh_task_ext(uint32_t timeout,bool isr) {
 if(timeout||isr) usb_wait_requested++;
 if(stalled_register) {
  static volatile uint32_t reg;
  (void)usb_wait_reg(&reg,1,1,USB_WAIT_REG_US,USB_WAIT_AHB);
 } else advance(delay);
}
static void watchdog(void) {usb_wait_report();}
static void lg(const char *s) {usblog_add(s);}
''' + task)
        link = ['-Wl,-Bsymbolic'] if sys.platform.startswith('linux') else []
        subprocess.run(['cc', '-shared', '-fPIC', '-O1', '-fno-builtin', '-DHOST_TEST',
                        '-D_FORTIFY_SOURCE=0', '-Wall', '-Wextra', '-Werror', *link,
                        '-I' + str(ROOT), '-I' + str(ROOT / 'tests/stubs'), '-I' + str(ROOT / 'include'),
                        *[str(ROOT / s) for s in hostlib.SOURCES], str(helper),
                        '-o', str(cls.work / 'core.dylib')], check=True)

    def start(self, ready=True):
        self.card({'GMOS.ELF': test_m1_user.program()})
        self.assertEqual(self.c.autostart_run(b'/OS/GMOS.ELF'), 0)
        if ready:
            C.c_uint32.in_dll(self.c, 'sys_healths').value += 1
            self.c.os_handle(2, 0, 0)

    def test_four_second_kernel_stall_does_not_fault_os(self):
        self.start()
        self.c.advance(1_000_000)  # half the OS budget already spent
        self.c.host_delay(4_000_000)
        self.c.usb_task()
        self.assertEqual(C.c_uint.in_dll(self.c, "usb_wait_requested").value, 0)
        self.c.os_handle(2, 0, 0)
        self.assertTrue(self.c.os_running())
        size = C.c_uint32()
        self.assertIn(b'USB stall absorbed', self.c.usblog_text(C.byref(size)))
        # Repeated short USB passes must not erase time outside USB.
        self.c.host_delay(100)
        for _ in range(999):
            self.c.usb_task()
            self.c.advance(1000)
            self.c.os_handle(2, 0, 0)
            self.assertTrue(self.c.os_running())
        self.c.advance(1000)
        self.c.os_handle(2, 0, 0)
        self.assertFalse(self.c.os_running())
        self.assertIn(b'heartbeat missed 2 s', self.c.take_log())

    def test_readiness_excludes_usb_but_still_expires(self):
        self.start(ready=False)
        self.c.host_delay(4_000_000)
        for _ in range(3):
            self.c.usb_task()
            self.c.os_handle(2, 0, 0)
            self.assertTrue(self.c.os_running())
        self.c.advance(10_000_000)
        self.c.os_handle(2, 0, 0)
        self.assertFalse(self.c.os_running())
        self.assertIn(b'no readiness report', self.c.take_log())

    def test_ticks_and_health_keep_flowing_during_pending_control(self):
        self.start()
        self.c.host_delay(100)
        for tick in range(4000):
            self.c.usb_task()
            self.c.advance(900)
            if tick % 500 == 0:
                C.c_uint32.in_dll(self.c, 'sys_healths').value += 1
            self.c.os_handle(2, 0, 0)
            self.assertTrue(self.c.os_running())
        self.assertEqual(C.c_uint32.in_dll(self.c, 'os_starts').value, 1)
        self.assertNotIn(b'OS fault', self.c.take_log())

    def test_usb_exclusion_does_not_delay_sleep_deadline(self):
        self.start()
        C.c_uint64.in_dll(self.c, 'sys_wake_us').value = self.c.clock_us() + 1_000_000
        self.c.host_delay(4_000_000)
        self.c.usb_task()
        self.c.os_asleep.restype = C.c_bool
        self.assertFalse(self.c.os_asleep())
        self.assertEqual(C.c_uint64.in_dll(self.c, 'sys_wake_us').value, 0)
        self.c.os_handle(2, 0, 0)
        self.assertTrue(self.c.os_running())

    def test_stuck_register_returns_to_running_os(self):
        self.start()
        self.c.usb_wait_failed.restype = C.c_bool
        self.c.host_stuck_register()
        begin = self.c.clock_us()
        self.c.usb_task()
        self.assertLessEqual(self.c.clock_us() - begin, 105)
        self.assertTrue(self.c.usb_wait_failed())
        size = C.c_uint32()
        self.assertIn(b'DWC2 timeout AHB idle count=1', self.c.usblog_text(C.byref(size)))
        for tick in range(4000):
            self.c.usb_task()
            self.c.advance(1000)
            if tick % 500 == 0:
                C.c_uint32.in_dll(self.c, 'sys_healths').value += 1
            self.c.os_handle(2, 0, 0)
            self.assertTrue(self.c.os_running())
        self.assertEqual(C.c_uint32.in_dll(self.c, 'os_starts').value, 1)
        self.assertNotIn(b'OS fault', self.c.take_log())
