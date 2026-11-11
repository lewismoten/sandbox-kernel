# SPDX-License-Identifier: MIT
"""Compatibility identities need a reference outside the inventory itself."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
VID_PID = re.compile(r'\b[0-9a-f]{4}:[0-9a-f]{4}\b', re.I)


class DocsKeyboards(unittest.TestCase):
    def test_vid_pids_have_fixture_or_docs_table_evidence(self):
        inventory = ROOT / 'docs/tested-keyboards.md'
        identities = {value.upper() for value in VID_PID.findall(inventory.read_text())}
        self.assertTrue(identities, 'the compatibility inventory has no VID:PID entries')
        evidence = set()
        # Native C fixtures contain explicit USB device/log identities. Do not
        # scan this Python test or the inventory, which would allow self-citation.
        for fixture in (ROOT / 'tests').rglob('*.c'):
            evidence.update(value.upper() for value in VID_PID.findall(fixture.read_text()))
        for doc in (ROOT / 'docs').rglob('*.md'):
            if doc == inventory:
                continue
            for line in doc.read_text().splitlines():
                if line.startswith('|'):
                    evidence.update(value.upper() for value in VID_PID.findall(line))
        self.assertFalse(identities - evidence,
                         f'VID:PID entries lack a fixture or separate docs table: {sorted(identities - evidence)}')


if __name__ == '__main__':
    unittest.main()
