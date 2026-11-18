# ADR 0044 recorded reason: these hold the logic of four existing Python scripts (verify_agents_imports.py, verify_log_rules.py,
# log_call_census.py, gen_hwdump_vk_fields.py) to what it decides, until the unit of #2132 that ports each of them.
"""The decisions of four guard and generator scripts that ran as test-map targets with no test of their own.

verify_agents_imports: which AGENTS.md import lines are accepted. verify_log_rules: which Info lines in a hot path are guarded.
log_call_census: which Log( calls the census counts. gen_hwdump_vk_fields: what the generated include holds.
"""
import importlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parent.parent


sys.path.insert(0, str(TOOLS))


IMPORT_LINE = "@docs/standards/logging.md"
SENTENCE = "The logging standard below is part of this contract"


def agents_text(import_line=IMPORT_LINE, sentence=SENTENCE, before_section=True):
    head = f"# AGENTS.md\n\n{sentence}:\n\n{import_line}\n\n" if before_section else f"# AGENTS.md\n\n{sentence}:\n\n"
    tail = "" if before_section else f"## Later\n\n{import_line}\n"
    return head + "## Skills\n\ntext\n" + tail


class AgentsImportsTest(unittest.TestCase):
    def setUp(self):
        self.mod = importlib.import_module("verify_agents_imports")
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)
        (self.root / "docs" / "standards").mkdir(parents=True)
        (self.root / "docs" / "standards" / "logging.md").write_text("# logging\n")

    def errors(self, text):
        (self.root / "AGENTS.md").write_text(text, encoding="utf-8")
        return self.mod.check(self.root)

    def test_the_import_in_the_preamble_with_its_sentence_is_accepted(self):
        self.assertEqual(self.errors(agents_text()), [])

    def test_an_import_after_the_first_section_is_refused(self):
        errors = self.errors(agents_text(before_section=False))
        self.assertEqual(len(errors), 1)
        self.assertIn("has no `@docs/standards/logging.md` line", errors[0])

    def test_an_indented_import_is_refused(self):
        errors = self.errors(agents_text(import_line="  " + IMPORT_LINE))
        self.assertEqual(len(errors), 1)
        self.assertIn("unindented", errors[0])

    def test_an_import_inside_a_code_fence_is_refused(self):
        errors = self.errors(agents_text(import_line="```\n" + IMPORT_LINE + "\n```"))
        self.assertEqual(len(errors), 1)
        self.assertIn("outside a code fence", errors[0])

    def test_a_missing_binding_sentence_is_refused(self):
        errors = self.errors(agents_text(sentence="Some other sentence"))
        self.assertEqual(len(errors), 1)
        self.assertIn(SENTENCE, errors[0])

    def test_an_import_of_a_missing_file_is_refused(self):
        (self.root / "docs" / "standards" / "logging.md").unlink()
        errors = self.errors(agents_text())
        self.assertEqual(len(errors), 1)
        self.assertIn("does not exist", errors[0])

    def test_an_unreadable_agents_file_is_one_error(self):
        errors = self.mod.check(self.root)  # no AGENTS.md written
        self.assertEqual(len(errors), 1)
        self.assertIn("cannot read", errors[0])

    def test_the_preamble_stops_at_the_first_section_and_skips_fences(self):
        lines = self.mod.preamble("a\n```\nfenced\n```\nb\n## Section\nc\n")
        self.assertEqual(lines, ["a", "b"])

    def test_main_exits_0_for_a_good_root_and_1_for_a_bad_one(self):
        (self.root / "AGENTS.md").write_text(agents_text(), encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err), mock.patch.object(sys, "argv", ["x", "--root", str(self.root)]):
            self.assertEqual(self.mod.main(), 0)
        self.assertIn("agents-imports OK", out.getvalue())
        (self.root / "AGENTS.md").write_text(agents_text(sentence="nope"), encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err), mock.patch.object(sys, "argv", ["x", "--root", str(self.root)]):
            self.assertEqual(self.mod.main(), 1)
        self.assertIn("agents-imports: FAIL", err.getvalue())


GUARD = r"if \(t == 1\)|% 3750"


def hot_path_source(body):
    return "static void DispatchPerFrameWork() {\n" + body + "}\n\nvoid After() {\n  Log(EchoVR::LogLevel::Info, \"x\");\n}\n"


class LogRulesTest(unittest.TestCase):
    def setUp(self):
        self.mod = importlib.import_module("verify_log_rules")
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)

    def problems(self, text, name="tick.cpp"):
        (self.root / name).write_text(text, encoding="utf-8")
        found = []
        with mock.patch.object(self.mod, "REPO", self.root):
            self.mod.check_hot_path(name, "DispatchPerFrameWork", GUARD, found)
        return found

    def test_an_info_line_behind_a_first_call_guard_passes(self):
        body = "  if (t == 1) {\n    Log(EchoVR::LogLevel::Info, \"once\");\n  }\n"
        self.assertEqual(self.problems(hot_path_source(body)), [])

    def test_an_unguarded_info_line_is_named_with_its_line_number(self):
        body = "  Work();\n  Log(EchoVR::LogLevel::Info, \"every frame\");\n"
        found = self.problems(hot_path_source(body))
        self.assertEqual(len(found), 1)
        self.assertIn("tick.cpp:3: unguarded Info log inside DispatchPerFrameWork()", found[0])

    def test_a_guard_older_than_the_lookback_does_not_count(self):
        filler = "  Work();\n" * self.mod.LOOKBACK
        body = "  if (t == 1) {}\n" + filler + "  Log(EchoVR::LogLevel::Info, \"late\");\n"
        self.assertEqual(len(self.problems(hot_path_source(body))), 1)

    def test_a_guard_just_inside_the_lookback_counts(self):
        filler = "  Work();\n" * (self.mod.LOOKBACK - 1)
        body = "  if (t == 1) {}\n" + filler + "  Log(EchoVR::LogLevel::Info, \"near\");\n"
        self.assertEqual(self.problems(hot_path_source(body)), [])

    def test_debug_lines_and_comments_are_not_counted(self):
        body = "  Log(EchoVR::LogLevel::Debug, \"fine\");\n  // Log(EchoVR::LogLevel::Info, \"commented\");\n  * LogLevel::Info\n"
        self.assertEqual(self.problems(hot_path_source(body)), [])

    def test_only_the_hot_function_is_checked(self):
        # the Info line in After() is unguarded but outside the function the rule names
        self.assertEqual(self.problems(hot_path_source("  Work();\n")), [])

    def test_a_missing_function_is_a_problem_not_a_pass(self):
        found = self.problems("void Other() {\n}\n")
        self.assertEqual(len(found), 1)
        self.assertIn("not found", found[0])

    def test_main_fails_when_a_listed_hot_path_has_an_unguarded_info_line(self):
        (self.root / "tick.cpp").write_text(hot_path_source("  Log(EchoVR::LogLevel::Info, \"x\");\n"), encoding="utf-8")
        err = io.StringIO()
        with mock.patch.object(self.mod, "REPO", self.root), \
                mock.patch.object(self.mod, "HOT_PATHS", [("tick.cpp", "DispatchPerFrameWork", GUARD)]), redirect_stderr(err):
            self.assertEqual(self.mod.main(), 1)
        self.assertIn("log-rules: FAIL tick.cpp:2", err.getvalue())


def git_env():
    """The environment of a git run in a scratch repository: the caller's, without its GIT_* variables, plus a fixed identity."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update({"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"})
    return env


def run_git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, env=git_env())


CENSUS_FILES = {
    # counted: three printf calls (Info, Warning, an expression level)
    "src/runtime/a.cpp": (
        "void Log(EchoVR::LogLevel level, const char* f);\n"      # a definition, not a call
        "void F() {\n"
        "  Log(EchoVR::LogLevel::Info, \"a\");\n"
        "  Log(\n      EchoVR::LogLevel::Warning, \"b\");\n"
        "  Log(level, \"c\");\n"
        "  // Log(EchoVR::LogLevel::Info, \"commented\");\n"
        "  logger.Log(EchoVR::LogLevel::Info, \"member\");\n"
        "  ns::Log(EchoVR::LogLevel::Info, \"scoped\");\n"
        "  MyLog(EchoVR::LogLevel::Info, \"tail\");\n"
        "}\n"),
    "src/core/b.h": "inline void G() { Log(EchoVR::LogLevel::Error, \"e\"); ServerFatal(\"x\"); TeeFprintf(\"y\"); }\n",
    # excluded from the printf scope
    "src/quest/q.cpp": "void Q() { Log(EchoVR::LogLevel::Info, \"quest\"); }\n",
    "src/legacy/l.cpp": "void L() { Log(EchoVR::LogLevel::Info, \"legacy\"); }\n",
    "src/runtime/tests/t.cpp": "void T() { Log(EchoVR::LogLevel::Info, \"test\"); }\n",
    "src/runtime/compat/session_router.cpp": "void S() { Log(EchoVR::LogLevel::Info, \"router\"); }\n",
    "docs/not_source.cpp": "void D() { Log(EchoVR::LogLevel::Info, \"outside the scope roots\"); }\n",
    "src/runtime/readme.md": "Log(EchoVR::LogLevel::Info, \"not a source extension\")\n",
}


class LogCallCensusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = importlib.import_module("log_call_census")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.repo = pathlib.Path(cls._tmp.name)
        run_git(cls.repo, "init", "-q")
        for name, text in CENSUS_FILES.items():
            path = cls.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        run_git(cls.repo, "add", "-A")
        run_git(cls.repo, "commit", "-q", "-m", "fixture")

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def census(self):
        result = subprocess.run([sys.executable, str(TOOLS / "log_call_census.py")], cwd=self.repo, check=True, capture_output=True, env=git_env(),
                                text=True)
        return result.stdout.splitlines()

    def test_the_printf_count_covers_only_real_calls_in_scope(self):
        lines = self.census()
        self.assertIn("printf Log( calls outside src/quest/, src/runtime/tests/ and session_router.cpp: 4 in 2 files", lines)
        self.assertIn("  src/runtime 3", lines)
        self.assertIn("  src/core 1", lines)

    def test_the_level_of_each_call_is_read_from_the_text_after_it(self):
        lines = self.census()
        self.assertIn("  level Info 1", lines)
        self.assertIn("  level Warning 1", lines)  # the level on the next line of the call still counts
        self.assertIn("  level Error 1", lines)
        self.assertIn("  level expression 1", lines)  # Log(level, ...): not a literal level
        self.assertIn("  level Debug 0", lines)

    def test_the_other_writers_are_counted_in_scope(self):
        lines = self.census()
        self.assertIn("  ServerFatal 1", lines)
        self.assertIn("  TeeFprintf 1", lines)
        self.assertIn("  FatalError 0", lines)

    def test_the_session_router_is_counted_on_its_own_line(self):
        self.assertIn("  Router::Log definitions/calls in session_router.cpp 1", self.census())

    def test_comments_and_definitions_are_recognised(self):
        text = "void Log(a);\n// Log(b);\n  * Log(c);\nx = 1; // Log(d)\n  Log(e);\n"
        found = [m.start() for m in self.mod.PRINTF_LOG.finditer(text)]
        flags = [self.mod.is_comment_or_definition(text, start) for start in found]
        self.assertEqual(flags, [True, True, True, True, False])

    def test_the_revision_argument_names_the_tree_that_is_counted(self):
        out = subprocess.run([sys.executable, str(TOOLS / "log_call_census.py"), "HEAD"], cwd=self.repo, check=True,
                             capture_output=True, text=True, env=git_env()).stdout
        self.assertTrue(out.startswith("revision "))
        bad = subprocess.run([sys.executable, str(TOOLS / "log_call_census.py"), "no-such-rev"], cwd=self.repo, capture_output=True,
                             text=True, env=git_env())
        self.assertNotEqual(bad.returncode, 0)


VULKAN_H = """\
typedef struct VkPhysicalDeviceFeatures {
    VkBool32    robustBufferAccess;
    VkBool32    fullDrawIndexUint32;
} VkPhysicalDeviceFeatures;
typedef struct VkPhysicalDeviceLimits {
    uint32_t              maxImageDimension1D;
    float                 maxSamplerLodBias;
    uint32_t              maxComputeWorkGroupCount[3];
    VkSampleCountFlags    framebufferColorSampleCounts;
} VkPhysicalDeviceLimits;
"""


class HwdumpFieldsTest(unittest.TestCase):
    def setUp(self):
        self.mod = importlib.import_module("gen_hwdump_vk_fields")
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = pathlib.Path(self._tmp.name)

    def generate(self, header_text):
        header = self.dir / "vulkan_core.h"
        header.write_text(header_text, encoding="utf-8")
        out = self.dir / "fields.inc"
        with mock.patch.object(sys, "argv", ["gen", str(header), str(out)]):
            self.mod.main()
        return out.read_text(encoding="utf-8")

    def test_members_come_out_with_their_type_name_and_array_length(self):
        members = self.mod.members(VULKAN_H, "VkPhysicalDeviceLimits")
        self.assertEqual(members, [("uint32_t", "maxImageDimension1D", None), ("float", "maxSamplerLodBias", None),
                                   ("uint32_t", "maxComputeWorkGroupCount", "3"),
                                   ("VkSampleCountFlags", "framebufferColorSampleCounts", None)])

    def test_the_include_lists_limits_with_arrays_and_features_as_x_macros(self):
        text = self.generate(VULKAN_H)
        self.assertEqual(text.splitlines(), [
            "// Generated by tools/gen_hwdump_vk_fields.py from the NDK's vulkan_core.h; do not edit.",
            "// VkPhysicalDeviceLimits: 4 members. VkPhysicalDeviceFeatures: 2 members.",
            "#define NEVR_HWDUMP_VK_LIMITS(X, XA) \\",
            "  X(maxImageDimension1D) \\",
            "  X(maxSamplerLodBias) \\",
            "  XA(maxComputeWorkGroupCount, 3) \\",
            "  X(framebufferColorSampleCounts)",
            "#define NEVR_HWDUMP_VK_FEATURES(X) \\",
            "  X(robustBufferAccess) \\",
            "  X(fullDrawIndexUint32)",
        ])

    def test_a_missing_struct_stops_the_generator_naming_it(self):
        with self.assertRaises(SystemExit) as stop:
            self.mod.members("typedef struct Other { int a; } Other;", "VkPhysicalDeviceLimits")
        self.assertIn("struct VkPhysicalDeviceLimits not found", str(stop.exception))

    def test_a_member_line_it_cannot_parse_stops_the_generator_naming_the_line(self):
        text = "typedef struct VkPhysicalDeviceLimits {\n    uint32_t a, b;\n} VkPhysicalDeviceLimits;\n"
        with self.assertRaises(SystemExit) as stop:
            self.mod.members(text, "VkPhysicalDeviceLimits")
        self.assertIn("cannot parse member line 'uint32_t a, b'", str(stop.exception))

    def test_the_wrong_argument_count_prints_the_usage(self):
        for argv in (["gen"], ["gen", "in.h"], ["gen", "in.h", "out.inc", "extra"]):
            with mock.patch.object(sys, "argv", argv), self.assertRaises(SystemExit) as stop:
                self.mod.main()
            self.assertIn("usage:", str(stop.exception))


if __name__ == "__main__":
    unittest.main()
