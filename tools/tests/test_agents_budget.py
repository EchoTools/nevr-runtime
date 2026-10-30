"""AGENTS.md states only what binds: it stays within a line budget and names no machinery the repo lacks (#1313).

The budget is the file's length when it was set, plus the lines the open contract PRs #1277, #1282, #1283 and #1302
add (4 + 12 + 9 + 7). A change that needs more lines takes them from text that no longer binds, or raises
LINE_BUDGET in its own diff and says in its PR body why the file must grow.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINE_BUDGET = 1406  # raised from 1394 by #1736 (the fixed clean build checkout rule, 6 lines); AGENTS.md was 1400 at origin/main 87d66bef, already over

# Names from the C++ addendum's former text that describe machinery this repo does not have, or a C++ standard above
# the one CMakeLists.txt sets (17). The removed text is kept in #1314.
ABSENT = {
    "flight recorder": "the flight recorder does not exist",
    "TraceEvent": "the flight recorder does not exist",
    "Sign all release DLLs": "release files are signed by the signer pipeline (RELEASING.md)",
    "${VCPKG_ROOT}/scripts/buildsystems": "CMakePresets.json reads vcpkg from $env{HOME}/.vcpkg",
    "trace dump": "no DLL writes one",
    "osslsigncode for Authenticode signing": "release files are signed by the signer pipeline (RELEASING.md)",
    "`just sign` to release process": "release files are signed by the signer pipeline (RELEASING.md)",
    "dxgi_dump": "the flight recorder does not exist",
    "LOG_INFO(": "logging is Log() under docs/standards/logging.md",
    "CODESIGN_PFX": "the release build reads no signing secret (RELEASING.md)",
    "├── gamepatches/": "the layout of another project: src/ has no gamepatches/, gameserver/ or common/",
    "src/gamepatches/": "src/ has no gamepatches/; the runtime is src/runtime/",
    "3uz": "a C++23 literal",
    "std::expected": "C++23",
    "std::span": "C++20",
    "std::thread_pool": "in no standard",
    "crash handler plugin": "plugins/crash-handler is not built",
}


class AgentsBudget(unittest.TestCase):
    def setUp(self):
        self.text = (ROOT / "AGENTS.md").read_text(encoding="utf-8")

    def test_agents_md_is_within_its_line_budget(self):
        lines = len(self.text.splitlines())
        self.assertLessEqual(lines, LINE_BUDGET,
                             f"AGENTS.md has {lines} lines, over its budget of {LINE_BUDGET}: cut text that no longer "
                             "binds, or raise LINE_BUDGET in this diff and say why in the PR body")

    def test_agents_md_names_no_absent_machinery(self):
        for name, why in ABSENT.items():
            with self.subTest(name=name):
                self.assertNotIn(name, self.text, why)

    def test_the_cxx_standard_is_still_17(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, r"set\(CMAKE_CXX_STANDARD 17\)",
                         "the C++17 names in ABSENT are wrong for another standard; revisit them")
        self.assertIsNone(re.search(r"set\(CMAKE_CXX_STANDARD (2\d)\)", cmake))

    def test_logging_and_signing_point_at_their_documents(self):
        flat = " ".join(self.text.split())
        self.assertIn("Logging follows `docs/standards/logging.md`", flat)
        self.assertIn("by the signer pipeline `RELEASING.md` describes; the release build signs nothing", flat)
        self.assertIn("`just sign` signs a local `dist/` with the private CA in `certs/`, for development builds only", flat)


if __name__ == "__main__":
    unittest.main()
