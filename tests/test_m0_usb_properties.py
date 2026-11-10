# SPDX-License-Identifier: MIT
"""Deterministic sender/model/real-C PUT2 framing checks; no physical USB."""
import contextlib
import ctypes as C
import hashlib
import io
import pathlib
import random
import re
import struct
import sys
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tests'))
import image
import usb_kernel
import test_m0


class ReceiverModel:
    """Independent stop-and-wait wire oracle, not image.validate or C code."""
    def __init__(self):
        self.sent = []
        self.replies = []
        self.data = bytearray()
        self.size = None
        self.crc = None
        self.final = None

    def send(self, data):
        self.sent.append(data)
        if self.size is not None:
            if not data or len(data) != min(1024, self.size - len(self.data)):
                raise AssertionError('sender violated stop-and-wait block length')
            self.data.extend(data)
            if len(self.data) < self.size:
                self.replies.append(f'ACK {len(self.data)}')
            else:
                self.replies.append('VERIFY')
                b = bytes(self.data)
                if zlib.crc32(b) != self.crc:
                    self.final = 'ERR crc'
                elif not self.manifest(b):
                    self.final = 'ERR manifest'
                else:
                    self.final = 'OK pending rebooting'
        elif data == b'INFO\n':
            self.replies.append('INFO 2 test confirmed=1 updates=ready os=none features=kernel,no-os')
        else:
            match = re.fullmatch(rb'PUT2 kernel zero-w-armv6 ([0-9]{1,10}) ([0-9a-f]{8})\n', data)
            if not match:
                raise AssertionError(f'invalid sender command: {data!r}')
            size, crc = int(match[1]), int(match[2], 16)
            if not 64 <= size <= 2097152:
                self.replies.append('ERR size')
            else:
                self.size, self.crc = size, crc
                self.replies.append('READY 1024')

    @staticmethod
    def manifest(b):
        if not 260 <= len(b) <= 2097152:
            return False
        expected = (0x31494b53, 1, 1, 0x10000, len(b)-256, 0x100000, 0x100000, 0)
        if struct.unpack_from('<8I', b) != expected or any(b[96:256]):
            return False
        build = b[64:96]
        nul = build.find(b'\0')
        return (nul >= 0 and not any(build[nul:]) and
                all(32 <= c <= 126 for c in build[:nul]) and
                hashlib.sha256(b[256:]).digest() == b[32:64])

    def reply(self, timeout):
        if self.replies:
            return self.replies.pop(0)
        if self.final is not None:
            reply, self.final = self.final, None
            return reply
        raise TimeoutError('model has no reply: incomplete transfer')


class UsbProperties(unittest.TestCase):
    # Reuse setup/helpers without inheriting all existing CoreTests test methods.
    setUpClass = classmethod(test_m0.CoreTests.setUpClass.__func__)
    tearDownClass = classmethod(test_m0.CoreTests.tearDownClass.__func__)
    setUp = test_m0.CoreTests.setUp
    tearDown = test_m0.CoreTests.tearDown
    confirmed_hash = test_m0.CoreTests.confirmed_hash
    cmd = test_m0.CoreTests.cmd
    begin = test_m0.CoreTests.begin
    transfer = test_m0.CoreTests.transfer
    install = test_m0.CoreTests.install

    def assert_uncommitted(self):
        self.assertEqual(self.c.write_count(), 0)
        ctl = (C.c_uint32 * 128).in_dll(self.c, 'control')
        self.assertEqual((ctl[2], ctl[4], ctl[7]), (1, 0xffffffff, 0))

    def reset_receive(self):
        self.c.protocol_reset()
        self.c.tud_cdc_read_flush()
        self.c.take_output()

    def check_bridge(self, size):
        seed = 0x55534232 ^ size
        rng = random.Random(seed)
        wire = image.pack(rng.randbytes(size-256), 'bridge')
        path = self.work / f'bridge-{size}.ski'
        path.write_bytes(wire)
        outer = self

        class Port(ReceiverModel):
            def send(port, data):
                super().send(data)
                actual = ''
                # Arbitrary CDC fragmentation must not change block ACK framing.
                at = 0
                while at < len(data):
                    n = rng.randint(1, min(197, len(data)-at))
                    actual += outer.cmd(data[at:at+n])
                    at += n
                wanted = port.replies[-1] + '\n'
                if data == b'INFO\n':
                    outer.assertTrue(actual.startswith('INFO 2 '), actual)
                    outer.assertIn(' updates=ready os=', actual);outer.assertIn(' features=kernel,no-os', actual)
                else:
                    outer.assertEqual(actual, wanted, f'seed={seed:#x} size={size}')

            def reply(port, timeout):
                wanted = super().reply(timeout)
                if wanted.startswith(('OK ', 'ERR ')):
                    outer.assertEqual(outer.install(), wanted+'\n')
                return wanted

        port = Port()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(usb_kernel.send_kernel(port, path), 0)
        self.assertEqual(bytes(port.data), wire)
        self.assertEqual(sum(s.startswith(b'PUT2 ') for s in port.sent), 1)
        ctl = (C.c_uint32 * 128).in_dll(self.c, 'control')
        self.assertEqual((ctl[2], ctl[4], ctl[7]), (2, 1, 2))
        self.assertEqual(bytes(ctl)[96:128], hashlib.sha256(wire).digest())

    def test_sender_real_receiver_minimum(self):
        self.check_bridge(260)

    def test_sender_real_receiver_exact_block(self):
        self.check_bridge(1024)

    def test_sender_real_receiver_short_final(self):
        self.check_bridge(1025)

    def test_sender_real_receiver_multiple_blocks(self):
        self.check_bridge(3072)

    def test_sender_model_seeded_block_boundaries(self):
        seed = 0x55534250
        rng = random.Random(seed)
        lengths = [260, 1023, 1024, 1025, 2047, 2048, 2049, 2097152]
        lengths += [rng.randint(260, 16384) for _ in range(128)]
        path = self.work / 'model.ski'
        for iteration, size in enumerate(lengths):
            with self.subTest(seed=hex(seed), iteration=iteration, size=size):
                wire = image.pack(rng.randbytes(size-256), 'model')
                path.write_bytes(wire)
                port = ReceiverModel()
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(usb_kernel.send_kernel(port, path), 0)
                self.assertEqual(bytes(port.data), wire)
                self.assertEqual(port.replies, [])
                self.assertIsNone(port.final)
        self.assert_uncommitted()

    def test_sender_rejects_invalid_images_before_any_io(self):
        path = self.work / 'bad.ski'
        bad_hash = bytearray(self.known)
        bad_hash[32] ^= 1
        bad_payload = bytearray(self.known)
        bad_payload[-1] ^= 1
        cases = [self.known[:n] for n in (0, 63, 64, 255, 259, 1023, len(self.known)-1)]
        cases += [bytes(bad_hash), bytes(bad_payload), self.known+b'X', bytes(2097153)]
        for iteration, wire in enumerate(cases):
            with self.subTest(iteration=iteration, size=len(wire)):
                path.write_bytes(wire)
                port = ReceiverModel()
                with self.assertRaises(ValueError):
                    usb_kernel.send_kernel(port, path)
                self.assertEqual(port.sent, [])
        self.assert_uncommitted()

    def test_truncated_blocks_timeout_without_writes(self):
        wire = image.pack(b'T'*1793, 'truncated')
        for cut in (0, 1, 63, 64, 259, 1023, 1024, 1025, 2047, 2048):
            with self.subTest(cut=cut):
                self.reset_receive()
                self.assertEqual(self.begin(wire), 'READY 1024\n')
                at = 0
                while at < cut:
                    end = min(at+1024, cut)
                    reply = self.cmd(wire[at:end])
                    self.assertEqual(reply, f'ACK {end}\n' if end % 1024 == 0 else '')
                    at = end
                self.c.advance(15000001)
                self.assertEqual(self.cmd(b''), 'ERR timeout\n')
                self.assert_uncommitted()
        # A canceled partial receive must not poison the next handshake.
        self.assertTrue(self.cmd('INFO\n').startswith('INFO 2 '))

    def test_oversize_and_overflow_commands_never_ready(self):
        for size in (0, 1, 63, 2097153, 4294967295, 4294967296, 10**30):
            with self.subTest(size=size):
                self.reset_receive()
                self.cmd('INFO\n')
                reply = self.cmd(f'PUT2 kernel zero-w-armv6 {size} 00000000\n')
                self.assertTrue(reply.startswith('ERR '), reply)
                self.assert_uncommitted()

    def test_bad_crc_and_hashes_against_model(self):
        seed = 0x55534248
        rng = random.Random(seed)
        for iteration in range(96):
            wire = bytearray(image.pack(rng.randbytes(rng.randint(4, 3000)), 'bad-hash'))
            kind = iteration % 3
            if kind == 1:
                wire[rng.randrange(32, 64)] ^= 1 << rng.randrange(8)
            elif kind == 2:
                wire[rng.randrange(256, len(wire))] ^= 1 << rng.randrange(8)
            wire = bytes(wire)
            crc = zlib.crc32(wire) ^ (1 if kind == 0 else 0)
            with self.subTest(seed=hex(seed), iteration=iteration, kind=kind):
                self.reset_receive()
                model = ReceiverModel()
                model.send(b'INFO\n'); model.reply(1)
                self.cmd('INFO\n')
                command = f'PUT2 kernel zero-w-armv6 {len(wire)} {crc:08x}\n'.encode()
                model.send(command)
                self.assertEqual(self.cmd(command), model.reply(1)+'\n')
                for at in range(0, len(wire), 1024):
                    block = wire[at:at+1024]
                    model.send(block)
                    self.assertEqual(self.cmd(block), model.reply(1)+'\n')
                self.assertEqual(self.install(), model.reply(1)+'\n')
                self.assert_uncommitted()

    def test_sender_stops_on_wrong_ready_ack_or_verify(self):
        wire = image.pack(b'F'*1792, 'bad-reply')
        path = self.work / 'bad-reply.ski'
        path.write_bytes(wire)
        for fault in ('READY 1024', 'ACK 1024', 'VERIFY'):
            with self.subTest(fault=fault):
                class Port(ReceiverModel):
                    def reply(port, timeout):
                        reply = super().reply(timeout)
                        return 'ERR injected-reply' if reply == fault else reply
                port = Port()
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(ValueError):
                    usb_kernel.send_kernel(port, path)
                blocks = [s for s in port.sent if not s.startswith((b'INFO\n', b'PUT2 '))]
                self.assertEqual(len(blocks), {'READY 1024': 0, 'ACK 1024': 1, 'VERIFY': 2}[fault])
                self.assertEqual(sum(s.startswith(b'PUT2 ') for s in port.sent), 1)
        self.assert_uncommitted()

    def test_sender_lost_final_is_unknown_without_resend(self):
        wire = image.pack(b'L'*4, 'lost-final')
        path = self.work / 'lost-final.ski'
        path.write_bytes(wire)
        for error in (TimeoutError, OSError):
            with self.subTest(error=error.__name__):
                class Port(ReceiverModel):
                    def reply(port, timeout):
                        if port.final is not None and not port.replies:
                            raise error('injected disconnect after VERIFY')
                        return super().reply(timeout)
                port = Port()
                diagnostic = io.StringIO()
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(diagnostic):
                    self.assertEqual(usb_kernel.send_kernel(port, path), 2)
                self.assertIn('Final result unknown', diagnostic.getvalue())
                self.assertIn('Do not resend blindly', diagnostic.getvalue())
                self.assertEqual(sum(s.startswith(b'PUT2 ') for s in port.sent), 1)
                self.assertEqual(bytes(port.data), wire)
        self.assert_uncommitted()

    def test_surplus_payload_rejected_before_commit(self):
        wire = image.pack(b'X'*769, 'surplus')
        self.assertEqual(self.begin(wire), 'READY 1024\n')
        self.transfer(wire)
        self.c.feed(b'Z', 1)
        self.assertEqual(self.install(), 'ERR trailing-input\n')
        self.assert_uncommitted()


if __name__ == '__main__':
    unittest.main()
