"""The script redirect's nine stock hashes are one list: the q15t plugin's (script_redirect.h) and the repack's staging
(quest_repack_libs.COMBAT_REDIRECT_HASHES). The host's registry must also hold all nine (#1737)."""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import quest_repack_libs as libs  # noqa: E402

PLUGIN_HEADER = REPO / "src/quest/plugins/q15t/src/script_redirect.h"
HOST_HEADER = REPO / "src/quest/plugin/script_redirect.h"
JUSTFILE = REPO / "justfile"


class ScriptRedirectListTest(unittest.TestCase):
    def test_the_plugin_and_the_staging_name_the_same_nine_hashes_in_the_same_order(self):
        block = re.search(r"kScriptRedirectHashes\[\]\s*=\s*\{(.*?)\};", PLUGIN_HEADER.read_text(encoding="utf-8"), re.S)
        self.assertIsNotNone(block, "kScriptRedirectHashes not found in the q15t plugin header")
        found = re.findall(r"0x([0-9a-f]{16})ULL", block.group(1))
        self.assertEqual(found, list(libs.COMBAT_REDIRECT_HASHES))
        self.assertEqual(len(found), 9)

    def test_the_host_registry_holds_all_nine(self):
        match = re.search(r"kMaxEntries\s*=\s*(\d+)", HOST_HEADER.read_text(encoding="utf-8"))
        self.assertIsNotNone(match, "kMaxEntries not found in the host header")
        self.assertGreaterEqual(int(match.group(1)), len(libs.COMBAT_REDIRECT_HASHES))

    def test_the_repack_recipe_collects_the_redirected_names_as_well_as_the_plain_ones(self):
        self.assertIn("lib????????????????_q15t.so", JUSTFILE.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
