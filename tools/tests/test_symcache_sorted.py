"""The generated symbol cache is sorted by ascending unsigned hash and has no duplicate hash.

patch/ui_texture_mips.cpp looks a texture's name up by binary search (nevr_ui_texture_mips::NameForHash); an unsorted
table makes that lookup miss names silently, so a regeneration that breaks the order fails here.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SYMCACHE = REPO / "src" / "runtime" / "log" / "symcache_data.cpp"
ENTRY = re.compile(r'^\s*\{0x([0-9a-fA-F]+)ULL?,\s*"', re.MULTILINE)
COUNT = re.compile(r"kSymCacheCount\s*=\s*(\d+)\s*;")


class SymcacheSortedTest(unittest.TestCase):
    def setUp(self):
        self.text = SYMCACHE.read_text()
        self.hashes = [int(h, 16) for h in ENTRY.findall(self.text)]

    def test_the_table_is_read(self):
        self.assertGreater(len(self.hashes), 1000)

    def test_the_declared_count_is_the_number_of_entries(self):
        declared = COUNT.search(self.text)
        self.assertIsNotNone(declared)
        self.assertEqual(int(declared.group(1)), len(self.hashes))

    def test_hashes_ascend_strictly(self):
        for before, after in zip(self.hashes, self.hashes[1:]):
            self.assertLess(before, after, f"0x{before:016x} is not before 0x{after:016x}")


if __name__ == "__main__":
    unittest.main()
