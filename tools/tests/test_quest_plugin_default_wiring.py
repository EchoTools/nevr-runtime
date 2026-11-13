"""Wiring of the packaged-plugin manifest on Quest (docs/adr/0019-quest-plugin-loader.md): the runtime reads the manifest and
merges it with config.yaml before it plans; the new source is built by the host test and the sentinel; the generic repack
tool names no project plugin.

Source-level checks: comments are stripped before matching, so prose cannot satisfy a check (N99, N111).
"""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class PackagedPluginWiringTest(unittest.TestCase):
    def read(self, rel: str) -> str:
        return (REPO / rel).read_text()

    def test_the_runtime_reads_and_merges_the_manifest_before_it_plans(self):
        code = strip_comments(self.read("src/quest/plugin/plugin_runtime.cpp"))
        self.assertIn("ParseManifest(", code)
        # The plan takes the merge as its argument, and the manifest is parsed before the plan is made.
        self.assertIn("loader.Plan(MergePackaged(", code)
        self.assertLess(code.index("ParseManifest("), code.index("loader.Plan("))

    def test_the_manifest_source_is_in_the_sentinel_build_and_the_host_test(self):
        self.assertIn("packaged_plugins.cpp", self.read("src/quest/integration/CMakeLists.txt"))
        plugins_cmake = self.read("src/quest/host_tests/groups/plugins.cmake")
        self.assertIn("src/quest/plugin/packaged_plugins.cpp", plugins_cmake)
        self.assertIn("src/quest/tests/packaged_plugins_test.cpp", plugins_cmake)

    def test_the_manifest_source_has_no_namespace_scope_object_with_a_constructor(self):
        code = strip_comments(self.read("src/quest/plugin/packaged_plugins.cpp"))
        # tools/check_quest_static_init.sh rejects these; a column-0 std container declared with an initialiser is one.
        bad = re.findall(r"^(?:inline\s+|static\s+)*std::(?:string|vector|map|set|unordered_map)\b[^(\n]*[=;{]", code, flags=re.M)
        self.assertEqual(bad, [])

    def test_the_generic_repack_tool_names_no_project_plugin(self):
        # The repack tool is Go (ADR 0044): its command and its library, not the tests beside them.
        for path in sorted((REPO / "tools" / "cmd" / "quest-repack-libs").glob("*.go")) + sorted(
                (REPO / "tools" / "internal" / "questrepack").glob("*.go")):
            if not path.name.endswith("_test.go"):
                self.assertNotIn("autopilot", path.read_text(encoding="utf-8").lower(), path.name)


if __name__ == "__main__":
    unittest.main()
