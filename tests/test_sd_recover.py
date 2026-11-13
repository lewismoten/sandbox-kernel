# SPDX-License-Identifier: MIT
"""SD driver error recovery on a modelled EMMC (tests/sd_sim.c): a failed read must not wedge later reads."""
import ctypes as C, pathlib, subprocess, tempfile, unittest
import hostlib

class SdRecover(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = tempfile.TemporaryDirectory(); so = pathlib.Path(cls.t.name)/'sd.so'
        subprocess.run(['cc','-shared','-fPIC','-O1','-DHOST_TEST','-Wall','-Wextra','-Werror','-I'+str(hostlib.ROOT/'include'),
                        str(hostlib.ROOT/'tests/sd_sim.c'),'-o',str(so)],check=True)
        cls.c = C.CDLL(str(so)); cls.c.sim_setup.argtypes=[C.c_uint32]; cls.c.sim_fail_reads.argtypes=[C.c_uint32]; cls.c.sim_cmd13_bad.argtypes=[C.c_uint32]; cls.c.sim_busy_after_write.argtypes=[C.c_uint32]; cls.c.sim_cmd13_lies.argtypes=[C.c_uint32]
    @classmethod
    def tearDownClass(cls): cls.t.cleanup()
    def setUp(self): self.c.sim_setup(1000); self.buf = C.create_string_buffer(512)
    def test_clean_read_works(self): self.assertTrue(self.c.sd_read(5, self.buf))
    def test_one_failed_read_is_retried_after_resetting_the_data_circuit(self):
        self.c.sim_fail_reads(1)
        self.assertTrue(self.c.sd_read(5, self.buf)); self.assertEqual(self.c.sim_cmd12s(), 1)
        self.assertTrue(self.c.sd_read(6, self.buf))
    def test_a_hard_failure_does_not_wedge_the_next_reads(self):
        self.c.sim_fail_reads(2)
        self.assertFalse(self.c.sd_read(5, self.buf))
        self.assertTrue(self.c.sd_read(6, self.buf))
        self.assertTrue(self.c.sd_read(7, self.buf))
    def test_failed_reinit_keeps_the_card_so_a_later_read_can_recover(self):
        self.c.sim_fail_reads(2); self.c.sim_cmd13_bad(1)  # recovery sees no transfer state, re-init fails (power property fails)
        self.assertFalse(self.c.sd_read(5, self.buf))
        self.assertGreater(self.c.sd_sectors(), 0, 'a failed re-init must not zero the capacity')
        self.assertTrue(self.c.sd_read(6, self.buf))
    def test_two_writes_then_reads_with_a_busy_card(self):
        self.c.sim_busy_after_write(40)  # DAT0 low for 40 polls after each write, like real flash programming
        for lba in (10, 11, 12): self.assertTrue(self.c.sd_write(lba, self.buf), 'write %d: %s' % (lba, C.c_char_p.in_dll(self.c, 'sd_fail').value))
        for lba in (10, 11): self.assertTrue(self.c.sd_read(lba, self.buf), 'read %d' % lba)
        self.assertEqual(self.c.sim_wedged(), 0, 'no command was sent while the card was busy')
    def test_dat0_is_waited_on_even_when_cmd13_says_ready(self):
        self.c.sim_busy_after_write(40); self.c.sim_cmd13_lies(1)  # only the DAT0 level shows the card is programming
        for lba in (10, 11, 12): self.assertTrue(self.c.sd_write(lba, self.buf), 'write %d' % lba)
        for lba in (10, 11): self.assertTrue(self.c.sd_read(lba, self.buf), 'read %d' % lba)
        self.assertEqual(self.c.sim_wedged(), 0)
    def test_out_of_range_is_refused_without_recovery(self):
        self.assertFalse(self.c.sd_read(5000, self.buf)); self.assertEqual(self.c.sim_cmd12s(), 0)

if __name__ == '__main__': unittest.main()
