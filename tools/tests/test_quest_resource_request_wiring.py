"""#871 and #1737 C5: the Quest resource request hook is in every build, its log lines are development-only, the game's
function runs first on a hit, and only ServeLoose writes the request.

The host tests (resource_request_hook_test, integration_sequence_test) run the handler and the sequence with fakes. What
they cannot see is the production wiring (the `Steps` answer is nevr_build::kIsDev, not a constant) and the properties of the
handler's source that no fake can fail: it calls the game before it looks at anything, the overlay is the only code that
writes the request, and the translation unit is built without exception support like every hook thunk.
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


def source_without_serve_loose():
    """The hook's source with the overlay's body removed: what is left must not write the request."""
    source = strip_comments(HOOK.read_text())
    serve = extract_braced_function(source, "Served ServeLoose(")
    assert "clearRequest(request)" in serve, "the body read is not ServeLoose (a sensor over nothing)"
    return source.replace(serve, "")


class TheHookIsInstalledInEveryBuild(unittest.TestCase):
    def test_the_production_answer_is_the_build_flag(self):
        source = (ROOT / "src/quest/sentinel/entry.cpp").read_text()
        self.assertRegex(strip_comments(source), r"bool\s+ResourceRequestLogEnabled\(\)\s*noexcept\s*\{\s*return\s+nevr_build::kIsDev\s*;\s*\}")

    def test_the_production_steps_ask_that_answer(self):
        source = strip_comments((ROOT / "src/quest/integration/production_steps.cpp").read_text())
        self.assertRegex(source, r"bool\s+ResourceRequestLogWanted\(\)\s*override\s*\{\s*return\s+nevr_quest::integration::ResourceRequestLogEnabled\(\)\s*;\s*\}")

    def test_the_sequence_has_no_release_skip_for_either_step(self):
        source = strip_comments((ROOT / "src/quest/integration/ctor_sequence.cpp").read_text())
        self.assertNotIn('r.Skip(StepId::kRegisterResourceRequestCounters, "release_build")', source)
        self.assertNotIn('r.Skip(StepId::kInstallResourceRequestLog, "release_build")', source)
        self.assertNotIn('r.Skip(StepId::kInstallResourceRequestLog, "counters_refused")', source)
        self.assertIn("steps.InstallResourceRequestLog(resourceCounters, resourceWanted)", source)
        self.assertLess(source.index("steps.ResourceRequestLogWanted()"), source.index("steps.RegisterResourceRequestCounters()"))

    def test_the_entry_passes_the_build_flag_only_as_the_log_flag(self):
        # entry.cpp holds kIsDev for other features too (the texture menu and probe options, PnsProviderEnabled), so the
        # count is not the pin. The pin is the path: the install never reads the build flag, it takes its log flag from the
        # argument, and the one read for this hook is ResourceRequestLogEnabled, the answer the production steps ask.
        source = strip_comments((ROOT / "src/quest/sentinel/entry.cpp").read_text())
        install = extract_braced_function(source, "bool InstallResourceRequestLogHook(")
        self.assertNotIn("kIsDev", install)
        self.assertIn("InstallCounted(countersRegistered, logLines)", install)
        answer = extract_braced_function(source, "bool ResourceRequestLogEnabled(")
        self.assertEqual(answer.count("kIsDev"), 1, answer)
        self.assertRegex(source, r"bool\s+InstallResourceRequestLogHook\(bool countersRegistered,\s*bool logLines\)\s*noexcept\s*\{\s*return\s+nevr_quest::resource_request::InstallCounted\(countersRegistered,\s*logLines\)\s*;")


class TheHandlerOnlyReadsOnAHit(unittest.TestCase):
    def test_the_game_is_called_on_every_path_that_is_not_served(self):
        body = handler_body()
        self.assertIn("original(manager, type, name, unused, allocator, owner, index, request)", body)

    def test_the_result_is_returned_as_the_game_gave_it(self):
        self.assertRegex(handler_body(), r"return ret;\s*\}$")

    def test_only_serve_loose_writes_the_request(self):
        rest = source_without_serve_loose()
        self.assertIsNone(re.search(r"\brequest\s*\[[^\]]*\]\s*(=(?!=)|\+=|-=|\|=|&=|\^=|<<=|>>=|\+\+|--)", rest))
        self.assertIsNone(re.search(r"\b(memcpy|memmove|memset|strcpy|strncpy)\s*\(\s*request\b", rest))
        self.assertIsNone(re.search(r"^\s*\*\s*\(?\s*request\b[^;\n]*[^=!<>]=[^=]", rest, re.M))
        self.assertNotIn("Store<", rest)

    def test_serve_loose_does_write_it_in_the_cave_order(self):
        serve = extract_braced_function(strip_comments(HOOK.read_text()), "Served ServeLoose(")
        order = [serve.index(s) for s in ("clearRequest(request)", "unpackagedPath(", "fns.open(", "fns.size(", "layout::kSize", "layout::kValid")]
        self.assertEqual(order, sorted(order))

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
        # The two thunk counters and the overlay's two (served, failed): all four are named in RegisterCounters.
        names = re.findall(r'\{"(resource_(?:request|overlay)_[a-z_]+)",', source)
        header = (ROOT / "src/quest/sentinel/resource_request_hook.h").read_text()
        count = int(re.search(r"kCounterCount\s*=\s*(\d+);", header).group(1))
        self.assertEqual(len(names), count, names)
        budget = (ROOT / "src/quest/tests/integration_hooks_test.cpp").read_text()
        self.assertRegex(budget, r"kSentinelCounters = [^;]*\+ " + str(count) + r";")


if __name__ == "__main__":
    unittest.main()
