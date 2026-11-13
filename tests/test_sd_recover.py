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
        cls.c = C.CDLL(str(so)); cls.c.sim_setup.argtypes=[C.c_uint32]; cls.c.sim_fail_reads.argtypes=[C.c_uint32]
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
    def test_out_of_range_is_refused_without_recovery(self):
        self.assertFalse(self.c.sd_read(5000, self.buf)); self.assertEqual(self.c.sim_cmd12s(), 0)

if __name__ == '__main__': unittest.main()
