# SPDX-License-Identifier: MIT
"""A USB card command that arrives while the kernel itself is writing the card (a log save, the bounded log
pump) must wait for that write: the board's SD driver services USB inside every transfer wait, so without the
wait the PUTF, RM or RUN ran inside the transfer in progress (the second file of a putgame, 2026-11-12)."""
import ctypes as C, hashlib, unittest, zlib
from test_m1_user import ProtocolBase, fsck, text

class PutInsideKernelCardWrite(ProtocolBase):
    def setUp(self):
        super().setUp()
        self.c.fatsave_put.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_uint32]
        self.c.fatlog_set.argtypes = [C.c_uint, C.c_char_p, C.c_uint32]
        self.c.nested_sd_ops.restype = C.c_uint32
        self.c.feed(b'INFO\n', 5); self.run_protocol(3); self.c.take_output()
    def queue_put(self, name, data):
        """The whole PUTF (command and body) is already in the CDC buffer, as when the host sends the next file
        of a folder right after the last one's OK."""
        line = f'PUTF {name} {len(data)} {zlib.crc32(data):08x} {hashlib.sha256(data).hexdigest()}\n'.encode()
        self.c.feed(line, len(line)); self.c.feed(data, len(data))
    def finish(self):
        out = self.c.take_output().decode()
        return out if 'OK file' in out or 'ERR' in out else out + self.run_protocol()
    def sound(self):
        a, b = self.fats(); self.assertEqual(a, b, 'the two FAT copies differ'); self.untouched()
        r = fsck(self.disk, self.work); self.assertEqual(r.returncode, 0, r.stdout + r.stderr); self.assertNotIn('Fix', r.stdout, r.stdout)

    def test_put_during_a_whole_file_log_save_waits_for_it(self):
        first = b'credits' * 3000; self.assertIn('OK file', self.put(first, 'GAMES/STARS/CREDITS.GIF'))
        log, second = b'usb log line\n' * 600, b'second file ' * 60
        self.queue_put('GAMES/STARS/CREDITS.LST', second)
        self.c.protocol_on_every_sd_op(1)
        r = self.c.fatsave_put(b'KERNEL', b'USB.TXT', log, len(log))     # the main loop's save; USB is serviced inside each SD wait
        self.c.protocol_on_every_sd_op(0)
        self.assertEqual(self.c.nested_sd_ops(), 0, 'a card command ran inside the save\'s SD transfer')
        self.assertEqual(r, 0, text(self.c, 'fatput_fail'))
        self.assertIn('OK file /GAMES/STARS/CREDITS.LST', self.finish())  # at the main-loop boundary
        self.assertEqual(self.read('/OS/SAVES/KERNEL/USB.TXT'), log)
        self.assertEqual(self.read('/GAMES/STARS/CREDITS.LST'), second)
        self.assertEqual(self.read('/GAMES/STARS/CREDITS.GIF'), first)
        self.sound()

    def test_put_during_the_log_pump_waits_for_the_step(self):
        log, second = b'12345 loop 900 passes\n' * 300, b'second file ' * 60
        self.assertEqual(self.c.fatlog_set(0, log, len(log)), 0)
        self.queue_put('GAMES/STARS/CREDITS.LST', second)
        self.c.protocol_on_every_sd_op(1)
        self.assertGreaterEqual(self.c.fatlog_pump(), 0, text(self.c, 'fatput_fail'))   # one pass: phase was 0 when it began
        self.c.protocol_on_every_sd_op(0)
        self.assertEqual(self.c.nested_sd_ops(), 0, 'a card command ran inside the pump\'s SD transfer')
        self.assertIn('OK file /GAMES/STARS/CREDITS.LST', self.finish())
        for _ in range(400):
            if not self.c.fatlog_pending(0): break
            self.assertGreaterEqual(self.c.fatlog_pump(), 0, text(self.c, 'fatput_fail'))
        self.assertFalse(self.c.fatlog_pending(0))
        self.assertEqual(self.read('/OS/SAVES/KERNEL/CEC.TXT')[:len(log)], log)
        self.assertEqual(self.read('/GAMES/STARS/CREDITS.LST'), second)
        self.sound()

    def test_rm_and_ls_during_a_save_wait_then_answer(self):
        data = b'x' * 3000; self.assertIn('OK file', self.put(data, 'GAMES/STARS/A.DAT'))
        self.c.feed(b'RM GAMES/STARS/A.DAT\n', 21)
        log = b'boot record\n' * 300
        self.c.protocol_on_every_sd_op(1)
        r = self.c.fatsave_put(b'KERNEL', b'USB.TXT', log, len(log))
        self.c.protocol_on_every_sd_op(0)
        self.assertEqual(self.c.nested_sd_ops(), 0); self.assertEqual(r, 0, text(self.c, 'fatput_fail'))
        self.assertEqual(self.c.take_output(), b'')                       # held, not refused
        self.assertIn('OK rm /GAMES/STARS/A.DAT', self.run_protocol(5))
        self.assertEqual(self.read('/OS/SAVES/KERNEL/USB.TXT'), log)
        self.c.feed(b'LS GAMES/STARS\n', 15)
        self.c.protocol_on_every_sd_op(1); r = self.c.fatsave_put(b'KERNEL', b'USB.TXT', log + b'more\n', len(log) + 5); self.c.protocol_on_every_sd_op(0)
        self.assertEqual(self.c.nested_sd_ops(), 0); self.assertEqual(r, 0, text(self.c, 'fatput_fail'))
        self.assertIn('OK ls /GAMES/STARS', self.run_protocol(5))
        self.sound()

    def test_keys_and_stop_are_still_parsed_inside_a_save(self):
        self.c.feed(b'INPUT add\n', 10)
        self.c.protocol_on_every_sd_op(1); r = self.c.fatsave_put(b'KERNEL', b'USB.TXT', b'a\n', 2); self.c.protocol_on_every_sd_op(0)
        self.assertEqual(r, 0); self.assertIn(b'OK input add', self.c.take_output())

if __name__ == '__main__': unittest.main()
