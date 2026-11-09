# SPDX-License-Identifier: MIT
"""The OS author's guide must cover the current ABI minor in abi.h: its section 4 table
needs a row for every minor from 1.0 through SBX_ABI_MINOR."""
import pathlib, re, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]

class DocsAbi(unittest.TestCase):
    def test_guide_table_reaches_current_minor(self):
        minor=int(re.search(r'#define\s+SBX_ABI_MINOR\s+(\d+)u',(ROOT/'sdk/include/sandbox/abi.h').read_text()).group(1))
        rows={int(m) for m in re.findall(r'^\| 1\.(\d+) \|',(ROOT/'docs/os-authors.md').read_text(),re.M)}
        self.assertEqual(set(range(minor+1)),rows&set(range(minor+1)),f'docs/os-authors.md section 4 is missing rows up to ABI 1.{minor}')
        self.assertLessEqual(max(rows),minor,'the guide lists an ABI minor newer than abi.h')
if __name__=='__main__': unittest.main()
