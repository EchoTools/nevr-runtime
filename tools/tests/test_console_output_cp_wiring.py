"""#1295: the console the runtime makes shows UTF-8, so an em dash in a Log() line reads as an em dash.

Source-order sensor. The console is made in PatchEnableHeadless (mode_patches.cpp) and nothing in the
tree can drive it without a Windows console, so this checks the call site, not the console's bytes:

  - the output code page is set to CP_UTF8 inside PatchEnableHeadless;
  - it is set after AllocConsole(), the call that creates the console it applies to;
  - it is set once in that function, not per log line.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def enable_headless_body() -> str:
    source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
    return extract_braced_function(source, "VOID PatchEnableHeadless(PVOID pGame)")


class HeadlessConsoleShowsUtf8(unittest.TestCase):
    def test_output_code_page_is_set_to_utf8(self):
        code = strip_comments(enable_headless_body())
        self.assertIn("SetConsoleOutputCP(CP_UTF8)", code)

    def test_it_is_set_after_the_console_is_allocated(self):
        code = strip_comments(enable_headless_body())
        self.assertIn("AllocConsole()", code)
        self.assertLess(code.index("AllocConsole()"), code.index("SetConsoleOutputCP(CP_UTF8)"))

    def test_it_is_set_once_in_the_function(self):
        code = strip_comments(enable_headless_body())
        self.assertEqual(code.count("SetConsoleOutputCP("), 1)


if __name__ == "__main__":
    unittest.main()
