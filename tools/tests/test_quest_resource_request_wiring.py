"""#871: the Quest resource request log is development-only, original-first and read-only.

The host tests (resource_request_hook_test, integration_sequence_test) run the handler and the sequence with fakes. What
they cannot see is the production wiring (the `Steps` answer is nevr_build::kIsDev, not a constant) and the properties of the
handler's source that no fake can fail: it calls the game before it looks at anything, it never writes the request, and the
translation unit is built without exception support like every hook thunk.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "src/quest/sentinel/resource_request_hook.cpp"


def handler_body():
    body = extract_braced_function(HOOK.read_text(), "std::uint64_t Handler(")
    assert "original(" in body, "the body read is not the handler (a sensor over nothing)"
    return body


class TheLogIsDevelopmentOnly(unittest.TestCase):
    def test_the_production_answer_is_the_build_flag(self):
        source = (ROOT / "src/quest/sentinel/entry.cpp").read_text()
        self.assertRegex(strip_comments(source), r"bool\s+ResourceRequestLogEnabled\(\)\s*noexcept\s*\{\s*return\s+nevr_build::kIsDev\s*;\s*\}")

    def test_the_production_steps_ask_that_answer(self):
        source = strip_comments((ROOT / "src/quest/integration/production_steps.cpp").read_text())
        self.assertRegex(source, r"bool\s+ResourceRequestLogWanted\(\)\s*override\s*\{\s*return\s+nevr_quest::integration::ResourceRequestLogEnabled\(\)\s*;\s*\}")

    def test_the_sequence_skips_both_steps_in_a_release_build(self):
        source = strip_comments((ROOT / "src/quest/integration/ctor_sequence.cpp").read_text())
        self.assertIn('r.Skip(StepId::kRegisterResourceRequestCounters, "release_build")', source)
        self.assertIn('r.Skip(StepId::kInstallResourceRequestLog, "release_build")', source)
        self.assertLess(source.index("steps.ResourceRequestLogWanted()"), source.index("steps.RegisterResourceRequestCounters()"))


class TheHandlerOnlyReads(unittest.TestCase):
    def test_the_game_is_called_before_anything_else(self):
        body = handler_body()
        self.assertRegex(body, r"^\{\s*const std::uint64_t ret = original\(manager, type, name, unused, allocator, owner, index, request\);")

    def test_the_result_is_returned_as_the_game_gave_it(self):
        self.assertRegex(handler_body(), r"return ret;\s*\}$")

    def test_nothing_in_the_file_writes_through_the_request(self):
        source = strip_comments(HOOK.read_text())
        self.assertIsNone(re.search(r"\brequest\s*\[[^\]]*\]\s*(=(?!=)|\+=|-=|\|=|&=|\^=|<<=|>>=|\+\+|--)", source))
        self.assertIsNone(re.search(r"\b(memcpy|memmove|memset|strcpy|strncpy)\s*\(\s*request\b", source))
        self.assertIsNone(re.search(r"^\s*\*\s*\(?\s*request\b[^;\n]*[^=!<>]=[^=]", source, re.M))

    def test_the_request_is_taken_as_a_pointer_to_unsigned_bytes(self):
        self.assertIn("unsigned char* request", HOOK.read_text())


class TheTranslationUnitIsBuiltWithoutExceptions(unittest.TestCase):
    def test_the_sentinel_build_lists_it(self):
        text = (ROOT / "src/quest/sentinel/CMakeLists.txt").read_text()
        line = next(l for l in text.splitlines() if l.startswith("set_source_files_properties(") and "-fno-exceptions" in l)
        self.assertIn("resource_request_hook.cpp", line)

    def test_the_integration_test_build_lists_it(self):
        text = (ROOT / "src/quest/integration/CMakeLists.txt").read_text()
        self.assertRegex(text, r'set_source_files_properties\("\$\{CMAKE_CURRENT_SOURCE_DIR\}/\.\./sentinel/resource_request_hook\.cpp"\s+PROPERTIES COMPILE_OPTIONS "-fno-exceptions"\)')


class TheCounterCountMatchesTheTable(unittest.TestCase):
    def test_the_counters_registered_are_the_count_the_budget_test_adds(self):
        source = strip_comments(HOOK.read_text())
        names = re.findall(r'\{"(resource_request_[a-z_]+)",\s*&FillThunk::(?:Call|Fault)Counter\(\)', source)
        header = (ROOT / "src/quest/sentinel/resource_request_hook.h").read_text()
        count = int(re.search(r"kCounterCount\s*=\s*(\d+);", header).group(1))
        self.assertEqual(len(names), count, names)
        budget = (ROOT / "src/quest/tests/integration_hooks_test.cpp").read_text()
        self.assertRegex(budget, r"kSentinelCounters = [^;]*\+ " + str(count) + r";")


if __name__ == "__main__":
    unittest.main()
