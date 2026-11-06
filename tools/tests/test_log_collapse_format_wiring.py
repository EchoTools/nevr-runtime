import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


class CollapseFormatWiring(unittest.TestCase):
    """E1 (STARBURST, N78). The behaviour is test_rate_limiter.cpp's (`RateLimiterRealdiv`, `CollapseFormatRules`). The built-in
    filter's decision before the limiter (`ShouldSuppress`, the default pattern list) is not driven by a test: builtin_filter.cpp
    hooks the game and links no test binary. These checks pin that a `realdiv(%x) divide by zero` line reaches the limiter, and that
    the limiter looks the format up."""

    def setUp(self):
        self.filter = (ROOT / "src/runtime/log/builtin_filter.cpp").read_text()
        self.limiter = (ROOT / "src/runtime/log/rate_limiter.h").read_text()
        self.rules = (ROOT / "src/runtime/log/collapse_format_rules.h").read_text()

    def test_no_default_suppress_pattern_deletes_the_realdiv_line(self):
        # A pattern that matches the line is a deletion: the line never reaches the limiter, so no count is written.
        defaults = extract_braced_function(self.filter, "static LogFilterConfig MakeDefaultConfig(")
        suppress = re.search(r"cfg\.suppress_patterns\s*=\s*\{(.*?)\n    \};", defaults, re.S)
        self.assertIsNotNone(suppress, "the default suppress list was not found")
        patterns = re.findall(r'"((?:[^"\\]|\\.)*)"', suppress.group(1))
        self.assertGreater(len(patterns), 20, "the list was read whole")
        line = "[EVR] realdiv(1a2b) divide by zero"
        deleting = [p for p in patterns if p in line]
        self.assertEqual(deleting, [], "a default pattern deletes the STARBURST divide-by-zero line instead of counting it")

    def test_the_limiter_identifies_a_line_by_its_format_rule_first(self):
        check = extract_braced_function(self.limiter, "RateVerdict Check(")
        self.assertIn("nevr_collapse_format::Match(msg, len)", check)
        self.assertRegex(check, r"format\s*!=\s*nullptr\s*\?\s*FormatHash\(format\)\s*:\s*NormalizedHash\(msg,\s*len\)")

    def test_the_filter_hands_the_limiter_the_game_line_whole(self):
        hook = extract_braced_function(self.filter, "static void __fastcall hook_PrintfImpl(")
        self.assertIn("g_rate.Check(buf, emit_len,", hook)
        self.assertLess(hook.index("ShouldSuppress(buf, effective, &entry)"), hook.index("g_rate.Check(buf, emit_len,"))

    def test_a_suppressed_line_is_counted_under_its_entry_in_the_limiter(self):
        # #2214: the deleting branch hands the limiter the matched entry; it never just bumps the total and returns.
        hook = extract_braced_function(self.filter, "static void __fastcall hook_PrintfImpl(")
        branch = hook[hook.index("ShouldSuppress(buf, effective, &entry)"):hook.index("int emit_len")]
        self.assertIn("g_rate.CountSuppressed(entry,", branch)
        self.assertIn("EmitLine(LOG_LEVEL_INFO, summary", branch)
        should = extract_braced_function(self.filter, "static bool ShouldSuppress(")
        self.assertEqual(should.count("return true;"), 3, "level gate, channel, pattern: only the last two name an entry")
        self.assertEqual(should.count("*entry = ch.c_str()") + should.count("*entry = pat.c_str()"), 2)
        self.assertIn("suppressed count=%u", self.limiter)

    def test_the_rule_table_names_the_script_format_and_is_a_listed_source(self):
        rules = strip_comments(self.rules)
        self.assertIn('{"script_realdiv", "realdiv(", ") divide by zero"}', rules)
        cmake = (ROOT / "src/runtime/CMakeLists.txt").read_text()
        self.assertIn('"log/collapse_format_rules.h"', cmake)


if __name__ == "__main__":
    unittest.main()
