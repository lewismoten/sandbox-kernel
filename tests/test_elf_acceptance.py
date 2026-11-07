# SPDX-License-Identifier: MIT
"""Deterministic ELF rejection witnesses, boundary checks and replayable fuzzing.

ELF_FUZZ_SEED changes the seed; ELF_FUZZ_CASES sets each campaign's budget
(defaults: mutations=60000, tables=40000). ELF_FUZZ_INDEX replays one index
in each campaign without walking preceding inputs. See docs/elf-host-coverage.md.
"""
import collections
import ctypes as C
import json
import os
import pathlib
import random
import re
import struct
import tempfile
import unittest

import hostlib
import test_m1_user as m1

PAGE = 4096
QUOTA = 8 << 20
MAX_FILE = 1 << 20
IGNORED = (0, 4, 6, 0x70000001, 0x70000003)
RELOC = (4, 9, 6, 11, 5, 0x6ffffff6)


def changed(blob, *fields):
    """(offset, struct format, value) edits, retaining all unrelated bytes."""
    out = bytearray(blob)
    for offset, fmt, value in fields:
        struct.pack_into('<' + fmt, out, offset, value)
    return bytes(out)


def single(**kw):
    return m1.elf([(m1.IMAGE, 5, m1.TEXT, 16)], **kw)


def rejection_cases():
    """Named witnesses, not probabilistic claims about reaching a rejection."""
    base = single()
    cases = []

    def add(name, reason, blob):
        cases.append((name, reason, blob))

    for n in (0, 1, 51):
        add('short-' + str(n), 'ELF SHORT', base[:n])
    add('too-big', 'ELF TOO BIG', base.ljust(MAX_FILE + 1, b'\0'))
    for i in range(4):
        add('magic-' + str(i), 'ELF MAGIC', changed(base, (i, 'B', 0)))
    for value, reason in ((2, 'ELF64'), (0, 'ELF CLASS'), (3, 'ELF CLASS')):
        add('class-' + str(value), reason, changed(base, (4, 'B', value)))
    for name, fields, reason in (
        ('endian', [(5, 'B', 2)], 'ELF ENDIAN'),
        ('ident-version', [(6, 'B', 0)], 'ELF VERSION'),
        ('header-version', [(20, 'I', 0)], 'ELF VERSION'),
        ('osabi', [(7, 'B', 3)], 'ELF OSABI'),
        ('pie', [(16, 'H', 3)], 'ELF PIE'),
        ('type', [(16, 'H', 1)], 'ELF TYPE'),
        ('machine', [(18, 'H', 183)], 'ELF MACHINE'),
        ('eabi', [(36, 'I', 0x04000200)], 'ELF EABI'),
        ('hard-float', [(36, 'I', 0x05000400)], 'ELF HARD FLOAT'),
        ('flags', [(36, 'I', 0x05000001)], 'ELF FLAGS'),
        ('ehsize', [(40, 'H', 51)], 'ELF HEADER SIZE'),
        ('phentsize', [(42, 'H', 31)], 'ELF HEADER SIZE'),
        ('phnum-zero', [(44, 'H', 0)], 'ELF PHNUM'),
        ('phnum-17', [(44, 'H', 17)], 'ELF PHNUM'),
        ('phnum-65535', [(44, 'H', 65535)], 'ELF PHNUM'),
        ('phoff-low', [(28, 'I', 48)], 'ELF PHDR RANGE'),
        ('phoff-truncated', [(28, 'I', len(base) - 28)], 'ELF PHDR RANGE'),
        ('phoff-unaligned', [(28, 'I', 53)], 'ELF PHDR RANGE'),
        ('phoff-overflow', [(28, 'I', 0xfffffff0)], 'ELF PHDR RANGE'),
        ('thumb', [(24, 'I', m1.IMAGE + 1)], 'ELF THUMB'),
        ('entry-align', [(24, 'I', m1.IMAGE + 2)], 'ELF ENTRY ALIGN'),
        ('seg-flags-high', [(76, 'I', 13)], 'ELF SEG FLAGS'),
        ('seg-flags-empty', [(76, 'I', 0)], 'ELF SEG FLAGS'),
        ('wx', [(76, 'I', 7)], 'ELF WX'),
        ('filesz', [(68, 'I', 17)], 'ELF FILESZ'),
        ('seg-file', [(56, 'I', len(base) - 15), (80, 'I', 1)], 'ELF SEG FILE'),
        ('seg-file-overflow', [(56, 'I', 0xfffffff8)], 'ELF SEG FILE'),
        ('align-power', [(80, 'I', 3)], 'ELF ALIGN'),
        ('align-congruence', [(56, 'I', PAGE + 1)], 'ELF ALIGN'),
        ('range-low', [(60, 'I', m1.IMAGE - PAGE)], 'ELF SEG RANGE'),
        ('range-high', [(60, 'I', m1.IMAGE_END)], 'ELF SEG RANGE'),
        ('range-overflow', [(60, 'I', 0xfffff000), (72, 'I', 0x2000)], 'ELF SEG RANGE'),
        ('entry-after-file', [(24, 'I', m1.IMAGE + 16)], 'ELF ENTRY'),
        ('entry-before-text', [(24, 'I', m1.IMAGE - 4)], 'ELF ENTRY'),
        ('entry-crosses-file', [(68, 'I', 15), (24, 'I', m1.IMAGE + 12)], 'ELF ENTRY'),
        ('entry-not-executable', [(76, 'I', 4)], 'ELF ENTRY'),
    ):
        add(name, reason, changed(base, *fields))
    for t, reason in ((2, 'ELF DYNAMIC'), (3, 'ELF INTERP'), (5, 'ELF SHLIB'),
                      (7, 'ELF TLS'), (0xffffffff, 'ELF PHDR TYPE')):
        add('type-' + str(t), reason, single(extra=[(t, 0)]))
    add('exec-stack', 'ELF EXEC STACK', single(extra=[(0x6474e551, 1)]))
    add('segments-five', 'ELF SEGMENTS', m1.elf([
        (m1.IMAGE + i * PAGE, 5 if i == 0 else 6, m1.TEXT, 16) for i in range(5)]))
    for delta in (0, 16, PAGE - 1):
        add('overlap-' + str(delta), 'ELF OVERLAP', m1.elf([
            (m1.IMAGE, 5, m1.TEXT, 16), (m1.IMAGE + delta, 6, b'x', 1)]))
    add('quota-rounded', 'ELF QUOTA', m1.elf([(m1.IMAGE, 5, m1.TEXT, QUOTA + 1)]))
    add('no-load-ignored', 'ELF NO LOAD', m1.elf([], extra=[(t, 0) for t in IGNORED]))
    add('no-load-empty', 'ELF NO LOAD', m1.elf([(m1.IMAGE, 5, b'', 0)]))
    sections = single(shdrs=[(0, 0)])
    for name, fields in (
        ('shentsize', [(46, 'H', 39)]),
        ('shnum', [(48, 'H', 257)]),
        ('shoff-align', [(32, 'I', 53)]),
        ('shoff-eof', [(32, 'I', len(sections) - 36)]),
        ('shoff-overflow', [(32, 'I', 0xfffffff0)]),
    ):
        add(name, 'ELF SHDR RANGE', changed(sections, *fields))
    for t in RELOC:
        add('reloc-' + str(t), 'ELF RELOC', single(shdrs=[(t, 2)]))
    return cases


class ElfAcceptance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        external = os.environ.get('ELF_TEST_LIBRARY')
        cls.c = C.CDLL(external or str(hostlib.build(cls.work / 'elf.so')))
        cls.c.elf_check.argtypes = [C.c_char_p, C.c_uint32, C.POINTER(m1.Plan)]
        cls.c.elf_check.restype = C.c_char_p
        cls.c.elf_load.argtypes = [C.c_char_p, C.POINTER(m1.Plan)]
        cls.c.us_reset.argtypes = [C.POINTER(C.c_uint32)]
        cls.c.us_map.argtypes = [C.c_uint32, C.c_uint32, C.c_uint32]
        cls.c.us_span.argtypes = [C.c_uint32, C.c_uint32, C.c_uint32, C.POINTER(C.c_void_p)]
        cls.c.us_span.restype = C.c_int64

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, blob):
        plan = m1.Plan()
        C.memset(C.byref(plan), 0xa5, C.sizeof(plan))
        result = self.c.elf_check(blob, len(blob), C.byref(plan))
        return result.decode() if result else None, plan

    def accepted(self, blob):
        reason, plan = self.check(blob)
        self.assertIsNone(reason)
        self.invariant(blob, plan)
        return plan

    def invariant(self, blob, plan):
        self.assertTrue(1 <= plan.count <= 4)
        pages = set()
        for seg in plan.seg[:plan.count]:
            self.assertTrue(m1.IMAGE <= seg.va < seg.va + seg.memsz <= m1.IMAGE_END)
            self.assertLessEqual(seg.filesz, seg.memsz)
            self.assertLessEqual(seg.offset + seg.filesz, len(blob))
            self.assertTrue(0 < seg.prot <= 7)
            self.assertFalse(seg.prot & 2 and seg.prot & 1)
            mapped = set(range(seg.va // PAGE, (seg.va + seg.memsz + PAGE - 1) // PAGE))
            self.assertFalse(pages & mapped)
            pages.update(mapped)
        self.assertEqual(plan.bytes, len(pages) * PAGE)
        self.assertLessEqual(plan.bytes, QUOTA)
        self.assertEqual(plan.entry % 4, 0)
        self.assertTrue(any(s.prot & 1 and s.va <= plan.entry and plan.entry + 4 <= s.va + s.filesz
                            for s in plan.seg[:plan.count]))

    def test_every_validator_rejection(self):
        cases = rejection_cases()
        # This inventory check detects new reason strings; execution and gcov,
        # not this static check, provide the actual path evidence.
        source = (hostlib.ROOT / 'core/elf.c').read_text()
        self.assertEqual(set(re.findall(r'"(ELF[^"\n]*)"', source)),
                         {reason for _, reason, _ in cases})
        for name, want, blob in cases:
            with self.subTest(witness=name):
                self.assertEqual(self.check(blob)[0], want)

    def test_null_and_short_reset_plan(self):
        for pointer, size in ((None, 52), (b'', 0), (b'x' * 51, 51)):
            plan = m1.Plan()
            C.memset(C.byref(plan), 0xa5, C.sizeof(plan))
            self.assertEqual(self.c.elf_check(pointer, size, C.byref(plan)), b'ELF SHORT')
            self.assertEqual(bytes(plan), bytes(C.sizeof(plan)))

    def test_maximal_program_headers(self):
        for count in (1, 15, 16):
            blob = single(extra=[(IGNORED[i % len(IGNORED)], 0) for i in range(count - 1)])
            self.assertEqual(self.accepted(blob).count, 1)
        segments = [(m1.IMAGE + i * PAGE, 5 if i == 0 else 6, m1.TEXT, 16) for i in range(4)]
        self.assertEqual(self.accepted(m1.elf(segments, extra=[(0, 0)] * 12)).count, 4)
        # The sixteenth header is inspected, not silently truncated.
        self.assertEqual(self.check(single(extra=[(0, 0)] * 14 + [(7, 0)]))[0], 'ELF TLS')
        # Non-empty loads after 15 ignored headers are still found.
        blob = bytearray(single(extra=[(0, 0)] * 15))
        blob[52:84], blob[52 + 15 * 32:52 + 16 * 32] = blob[52 + 15 * 32:52 + 16 * 32], blob[52:84]
        self.accepted(bytes(blob))

    def test_segment_page_edges_and_entry(self):
        for offset, size in ((0, 4095), (0, 4096), (0, 4097), (4, 4092),
                             (4, 4093), (4092, 4), (4092, 5)):
            with self.subTest(offset=offset, size=size):
                va = m1.IMAGE + offset
                blob = m1.elf([(va, 5, m1.TEXT[:4], size)], entry=va)
                p = self.accepted(blob)
                self.assertEqual(p.bytes, ((offset + size + PAGE - 1) // PAGE) * PAGE)
        self.accepted(m1.elf([(m1.IMAGE_END - 4, 5, b'ARM!', 4)], entry=m1.IMAGE_END - 4))
        self.assertEqual(self.check(m1.elf([(m1.IMAGE_END - 4, 5, b'ARM!', 5)],
                                         entry=m1.IMAGE_END - 4))[0], 'ELF SEG RANGE')
        self.accepted(m1.elf([(m1.IMAGE, 5, m1.TEXT, PAGE),
                              (m1.IMAGE + PAGE, 6, b'x', PAGE)]))
        self.assertEqual(self.check(m1.elf([(m1.IMAGE, 5, m1.TEXT, PAGE + 1),
                                          (m1.IMAGE + PAGE, 6, b'x', PAGE)]))[0], 'ELF OVERLAP')
        # Reverse VA order covers the non-overlap arm of the second comparison.
        self.accepted(m1.elf([(m1.IMAGE + PAGE, 6, b'x', PAGE),
                              (m1.IMAGE, 5, m1.TEXT, PAGE)]))
        for size, want in ((QUOTA - 1, None), (QUOTA, None), (QUOTA + 1, 'ELF QUOTA')):
            self.assertEqual(self.check(m1.elf([(m1.IMAGE, 5, m1.TEXT, size)]))[0], want)

    def test_section_and_table_boundaries(self):
        # Tables can cross a page; only alignment and file extent matter.
        for start in (PAGE - 40, PAGE - 4, PAGE, PAGE + 4):
            for count in (1, 256):
                blob = bytearray(single().ljust(start + count * 40, b'\0'))
                struct.pack_into('<I', blob, 32, start)
                struct.pack_into('<HH', blob, 46, 40, count)
                blob[start:start + count * 40] = bytes(count * 40)
                # Put text after the table, avoiding table/payload aliasing.
                off = ((len(blob) + PAGE - 1) // PAGE) * PAGE
                blob.extend(bytes(off + 4 - len(blob)))
                blob[off:off + 4] = b'ARM!'
                struct.pack_into('<I', blob, 56, off)
                struct.pack_into('<I', blob, 68, 4)
                self.accepted(bytes(blob))
                self.assertEqual(self.check(bytes(blob[:start + count * 40 - 1]))[0], 'ELF SEG FILE')
                # Move payload before table so truncation reaches the section guard.
                struct.pack_into('<I', blob, 56, 0)
                struct.pack_into('<I', blob, 80, 1)
                self.assertEqual(self.check(bytes(blob[:start + count * 40 - 1]))[0], 'ELF SHDR RANGE')
        for start in (PAGE - 32, PAGE - 4, PAGE, PAGE + 4):
            blob = bytearray(single().ljust(2 * PAGE + 4, b'\0'))
            header = bytearray(blob[52:84])
            struct.pack_into('<I', header, 4, 2 * PAGE)
            struct.pack_into('<I', header, 16, 4)
            struct.pack_into('<I', blob, 28, start)
            blob[start:start + 32] = header
            blob[2 * PAGE:2 * PAGE + 4] = b'ARM!'
            self.accepted(bytes(blob))
        for t in RELOC:
            self.accepted(single(shdrs=[(t, 0)]))
            self.assertEqual(self.check(single(shdrs=[(0, 0)] * 255 + [(t, 2)]))[0], 'ELF RELOC')
        # A zero-sized table at exact EOF is accepted; one byte beyond is not.
        blob = changed(single(), (32, 'I', len(single())), (46, 'H', 40))
        self.accepted(blob)
        self.assertEqual(self.check(changed(blob, (32, 'I', len(blob) + 4)))[0], 'ELF SHDR RANGE')

    def test_supported_optional_and_empty_fields(self):
        self.accepted(changed(single(), (7, 'B', 97), (36, 'I', 0x05000000)))
        for alignment in (0, 1, 2, 4, PAGE, 1 << 27):
            blob = single()
            if alignment > PAGE:
                blob = changed(blob, (56, 'I', 0), (68, 'I', 4))
            self.accepted(changed(blob, (80, 'I', alignment)))
        self.accepted(single(extra=[(0x6474e551, 6)]))
        # Empty loads are skipped before segment validation, per current format.
        self.accepted(single(extra=[(1, 0xffffffff)] * 15))
        self.accepted(single().ljust(MAX_FILE, b'\0'))
        self.assertEqual(self.check(changed(single(), (68, 'I', 0)))[0], 'ELF ENTRY')

    def reset_space(self):
        self.assertEqual(self.c.us_reset((C.c_uint32 * 4096)()), 0)

    def test_load_copy_bss_padding_and_map_failure(self):
        self.reset_space()
        payload = bytes(range(251)) * 17
        va = m1.IMAGE + PAGE - 4
        blob = m1.elf([(va, 5, payload, len(payload) + PAGE)], entry=va)
        plan = self.accepted(blob)
        self.assertEqual(self.c.elf_load(blob, C.byref(plan)), 0)
        ptr = C.c_void_p()
        self.assertEqual(self.c.us_span(m1.IMAGE, plan.bytes, 4, C.byref(ptr)), plan.bytes)
        got = C.string_at(ptr, plan.bytes)
        self.assertEqual(got[:PAGE - 4], bytes(PAGE - 4))
        self.assertEqual(got[PAGE - 4:PAGE - 4 + len(payload)], payload)
        self.assertEqual(got[PAGE - 4 + len(payload):], bytes(plan.bytes - PAGE + 4 - len(payload)))
        self.assertEqual(self.c.elf_load(blob, C.byref(plan)), -22)
        # A pure-BSS load after text takes the filesz==0 branch of elf_load.
        self.reset_space()
        blob = m1.elf([(m1.IMAGE, 1, m1.TEXT, 16), (m1.IMAGE + PAGE, 2, b'', PAGE + 1)])
        plan = self.accepted(blob)
        self.assertEqual(self.c.elf_load(blob, C.byref(plan)), 0)
        self.assertEqual(self.c.us_span(m1.IMAGE + PAGE, 2 * PAGE, 4, C.byref(ptr)), 2 * PAGE)
        self.assertEqual(C.string_at(ptr, 2 * PAGE), bytes(2 * PAGE))

    def campaign(self, kind, default):
        seed = int(os.environ.get('ELF_FUZZ_SEED', '1107'), 0)
        count = int(os.environ.get('ELF_FUZZ_CASES', str(default)))
        self.assertTrue(1 <= count <= 1000000, 'budget must be 1..1000000 per campaign')
        replay = os.environ.get('ELF_FUZZ_INDEX')
        indices = [int(replay)] if replay is not None else range(count)
        results = collections.Counter()
        trace = None
        if os.environ.get('ELF_FUZZ_TRACE'):
            trace = open(os.environ['ELF_FUZZ_TRACE'], 'a')
            self.addCleanup(trace.close)
        for index in indices:
            self.assertTrue(0 <= index < 1000000)
            rnd = random.Random((seed << 32) + index)
            blob = mutation_input(rnd) if kind == 'mutations' else table_input(rnd)
            if trace:
                trace.write(json.dumps(dict(kind=kind, seed=seed, index=index)) + '\n')
                trace.flush()  # Last record identifies even a crash/timeout input.
            try:
                reason, plan = self.check(blob)
                if reason is None:
                    self.invariant(blob, plan)
                results[reason or 'ACCEPT'] += 1
            except Exception:
                folder = pathlib.Path(os.environ.get('ELF_FAILURE_DIR', str(hostlib.ROOT / 'build/elf-failures')))
                folder.mkdir(parents=True, exist_ok=True)
                target = folder / f'elf-{kind}-{seed}-{index}.elf'
                target.write_bytes(blob)
                print(f'ELF failure: kind={kind} seed={seed} index={index} input={target}', flush=True)
                raise
        print('ELF_CAMPAIGN ' + json.dumps(dict(kind=kind, seed=seed, cases=sum(results.values()),
                                                results=dict(sorted(results.items()))), sort_keys=True), flush=True)
        if replay is None and count >= 1000:
            self.assertGreater(results['ACCEPT'], 0, 'campaign must exercise accepted plans')

    def test_fuzz_mutations_extended(self):
        self.campaign('mutations', 60000)

    def test_fuzz_tables_extended(self):
        self.campaign('tables', 40000)


def mutation_input(rnd):
    blob = bytearray(m1.program())
    for _ in range(rnd.randint(1, 8)):
        at = rnd.randrange(116)
        blob[at] = rnd.choice([0, 1, 0xff, 0x7f, rnd.getrandbits(8)])
    if rnd.randrange(3) == 0:
        struct.pack_into('<I', blob, rnd.randrange(29) * 4,
                         rnd.choice([0, 0xffffffff, 0x80000000, 0x0fffffff, rnd.getrandbits(32)]))
    if rnd.randrange(10) == 0:
        del blob[rnd.randrange(len(blob)):]
    return bytes(blob)


def table_input(rnd):
    # Half structured valid/near-valid; half unconstrained tables. Unlike the
    # old fuzzer, valid entries and the full 1..16 PHDR range are intentional.
    count = rnd.randint(1, 16)
    if rnd.randrange(2):
        nseg = rnd.randint(1, min(4, count))
        segs = []
        for i in range(nseg):
            va = m1.IMAGE + i * 3 * PAGE + rnd.choice([0, 4, PAGE - 4])
            segs.append((va, rnd.choice([1, 5]) if i == 0 else rnd.choice([2, 4, 6]),
                         b'ARM!' * rnd.randint(1, 8), rnd.choice([32, PAGE, PAGE + 1])))
        blob = m1.elf(segs, entry=segs[0][0], extra=[(rnd.choice(IGNORED), 0) for _ in range(count - nseg)],
                      shdrs=[(rnd.choice(RELOC), rnd.choice([0, 2]))] if rnd.randrange(4) == 0 else ())
        if rnd.randrange(4) == 0:
            blob = changed(blob, (52 + 32 * rnd.randrange(nseg) + rnd.choice([4, 8, 16, 20, 24, 28]),
                                  'I', rnd.getrandbits(32)))
        return blob
    vals = [0, 1, PAGE - 1, PAGE, m1.IMAGE, m1.IMAGE + PAGE, m1.IMAGE_END - PAGE,
            m1.IMAGE_END, 0x7fffffff, 0xfffff000, 0xffffffff]
    blob = bytearray(single(extra=[(0, 0)] * (count - 1)))
    for i in range(count):
        fields = [rnd.choice([1, 1, 0, 2, 3, 4, 5, 6, 7, 0x6474e551, 0x70000001, 9])]
        fields += [rnd.choice(vals + [rnd.getrandbits(32)]) for _ in range(5)]
        fields += [rnd.choice([4, 5, 6, 7, 1, 0, 8]), rnd.choice([0, 1, 4, PAGE, 3, 0x10000])]
        struct.pack_into('<8I', blob, 52 + i * 32, *fields)
    return changed(bytes(blob), (24, 'I', rnd.choice(vals)))


if __name__ == '__main__':
    unittest.main()
