"""#1965: the Quest manifest names the vcpkg revision the pin records, so a local configure resolves the same ports as CI.

Without a builtin-baseline, vcpkg resolves each port from whatever revision the local checkout carries. The pin file
.vcpkg-commit is the revision CI checks out (.github/workflows/build.yml). These checks read the two files and the
configure comment that describes the gap.
"""
from __future__ import annotations

import json
import pathlib
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
QUEST_MANIFEST = REPO / "src/quest/vcpkg.json"
PIN = REPO / ".vcpkg-commit"
QUEST_CMAKE = REPO / "src/quest/CMakeLists.txt"


class QuestVcpkgBaseline(unittest.TestCase):
    def test_the_quest_manifest_names_the_pinned_revision_as_its_builtin_baseline(self):
        manifest = json.loads(QUEST_MANIFEST.read_text(encoding="utf-8"))
        pinned = PIN.read_text(encoding="utf-8").strip()
        self.assertIn("builtin-baseline", manifest, "src/quest/vcpkg.json has no builtin-baseline")
        self.assertEqual(manifest["builtin-baseline"], pinned)

    def test_the_configure_comment_no_longer_says_the_manifest_has_no_baseline(self):
        text = QUEST_CMAKE.read_text(encoding="utf-8")
        self.assertNotIn("has no builtin-baseline", text)
        self.assertIn("builtin-baseline", text, "the comment should say the manifest pins its baseline")


if __name__ == "__main__":
    unittest.main()
