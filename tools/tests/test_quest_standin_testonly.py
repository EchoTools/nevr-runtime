"""The Quest stand-in test hooks must never ship in the production library.

StandIn::SetForTest / ResetForTest exist only to let the host tests fix or clear the per-process
stand-ins; ResetForTest unpublishes them, which makes every stand-in predicate answer false and so
disables the login send gate. They are compiled only under NEVR_QUEST_TESTING. This sensor proves,
from the source and the build graph (no Android toolchain needed), that:

  1. both functions in src/quest/login/login_standin.cpp are inside an #if defined(NEVR_QUEST_TESTING)
     block, and the header declares them the same way;
  2. no production target in src/quest/CMakeLists.txt defines NEVR_QUEST_TESTING (only test
     executables may), so the nevr_quest_login archive and libovrplatformloader.so are built without
     it and therefore contain neither symbol.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CPP = ROOT / "src/quest/login/login_standin.cpp"
HDR = ROOT / "src/quest/login/login_standin.h"
CMAKE = ROOT / "src/quest/CMakeLists.txt"


def _guarded(text: str, name: str) -> bool:
    """True iff every definition/declaration of `name` sits under NEVR_QUEST_TESTING."""
    # Find the #if defined(NEVR_QUEST_TESTING) ... #endif regions and check all hits fall inside one.
    regions = []
    depth = 0
    start = None
    for m in re.finditer(r"^#if(.*)$|^#endif.*$", text, re.MULTILINE):
        if m.group(0).startswith("#if"):
            if depth == 0 and "NEVR_QUEST_TESTING" in m.group(0):
                start = m.end()
                guard_depth = depth
            depth += 1
        else:
            depth -= 1
            if start is not None and depth == guard_depth:
                regions.append((start, m.start()))
                start = None
    # Only declarations/definitions/calls (name followed by '('); prose mentions have no paren.
    hits = [m.start() for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text)]
    return bool(hits) and all(any(a <= h <= b for a, b in regions) for h in hits)


class StandInTestOnly(unittest.TestCase):
    def test_setfortest_and_resetfortest_are_guarded_in_the_cpp(self):
        text = CPP.read_text()
        for name in ("SetForTest", "ResetForTest"):
            self.assertTrue(_guarded(text, name), f"{name} is not under NEVR_QUEST_TESTING in {CPP}")

    def test_header_guards_them_too(self):
        text = HDR.read_text()
        for name in ("SetForTest", "ResetForTest"):
            self.assertTrue(_guarded(text, name), f"{name} is not under NEVR_QUEST_TESTING in {HDR}")

    def test_no_production_target_defines_the_macro(self):
        text = CMAKE.read_text()
        # Every NEVR_QUEST_TESTING mention must be a target_compile_definitions on a *_test target.
        for m in re.finditer(r"NEVR_QUEST_TESTING", text):
            line = text[text.rfind("\n", 0, m.start()) + 1 : text.find("\n", m.start())]
            self.assertIn("target_compile_definitions", line, f"NEVR_QUEST_TESTING set outside a test target: {line!r}")
            self.assertIn("_test", line, f"NEVR_QUEST_TESTING set on a non-test target: {line!r}")


if __name__ == "__main__":
    unittest.main()
