"""The test shelf (tools/test_shelf.json) and the ceiling every Python test runs under (tools/shelf_reader.py,
tools/tests/conftest.py, tools/run_tool_tests.py, tools/unittest_ceiling.py).

ADR 0044 recorded reason: this tests the Python readers and runners of the shelf in the suite they run, and it also runs under
unittest where the verify recipe has no pytest; the Go half is tested in Go (internal/testshelf, cmd/gtest-deadline).
"""

import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_tool_tests  # noqa: E402
import shelf_reader  # noqa: E402

try:
    import conftest  # noqa: E402  (imports pytest)
except ModuleNotFoundError:  # the verify suite also runs under unittest on an image with no pytest (#820)
    conftest = None
HAS_TIMEOUT_PLUGIN = importlib.util.find_spec("pytest_timeout") is not None

OWNER_CEILING_S = 30
PROBE_CEILING_S = 1
PROBE_SLEEP_S = 8
PROBE_BUDGET_S = 25
RUN_BUDGET_S = 120
REPO = TOOLS.parent
ENTRY = {"id": "pytest:test_x", "reason": "median 40 s over 5 runs", "measured": "junit", "date": "2026-11-16"}


def shelf_with(*entries, ceiling=OWNER_CEILING_S):
    return {"schema": shelf_reader.SCHEMA, "ceiling_s": ceiling, "entries": list(entries)}


def entry(entry_id):
    return {**ENTRY, "id": entry_id}


class ShelfFileTest(unittest.TestCase):
    def test_the_justfiles_ceiling_is_the_shelfs(self):
        done = subprocess.run(["just", "--justfile", str(REPO / "justfile"), "--evaluate", "test_ceiling_s"],
                              capture_output=True, text=True, timeout=RUN_BUDGET_S, check=False)
        self.assertEqual(done.stdout.strip(), str(shelf_reader.ceiling_s()), done.stderr)

    def test_the_ceiling_is_the_owners_thirty_seconds(self):
        self.assertEqual(shelf_reader.ceiling_s(), OWNER_CEILING_S)

    def test_the_reader_and_the_go_reader_agree_on_the_entries_they_accept(self):
        shelf = shelf_reader.load()
        self.assertTrue(shelf["entries"], "nothing is shelved")
        for e in shelf["entries"]:
            shelf_reader.split_id(e["id"])

    def test_a_bad_shelf_is_refused(self):
        bad = {
            "a ceiling of 0": shelf_with(ceiling=0),
            "a ceiling past a minute": shelf_with(ceiling=shelf_reader.MAX_CEILING_S + 1),
            "an unknown kind": shelf_with(entry("perl:t")),
            "no reason": shelf_with({**ENTRY, "reason": " "}),
            "no date": shelf_with({**ENTRY, "date": ""}),
            "a case on a recipe": shelf_with(entry("just:t#c")),
            "the same id twice": shelf_with(ENTRY, ENTRY),
        }
        with tempfile.TemporaryDirectory() as d:
            for name, shelf in bad.items():
                path = pathlib.Path(d) / "shelf.json"
                path.write_text(json.dumps(shelf), encoding="utf-8")
                with self.assertRaises(shelf_reader.ShelfError, msg=name):
                    shelf_reader.load(path)
            path.write_text(json.dumps(shelf_with(ENTRY)), encoding="utf-8")
            self.assertEqual(shelf_reader.load(path)["ceiling_s"], OWNER_CEILING_S)


class SkipReasonTest(unittest.TestCase):
    def test_a_whole_file_a_class_and_a_test_can_be_shelved(self):
        shelf = shelf_with(entry("pytest:test_a"), entry("pytest:test_b#Slow"), entry("pytest:test_c#Slow.test_one"),
                           entry("pytest:test_d#test_function"))
        self.assertIn("shelved 2026-11-16", shelf_reader.skip_reason(shelf, "test_a", "Any", "test_any"))
        self.assertIsNotNone(shelf_reader.skip_reason(shelf, "test_b", "Slow", "test_x"))
        self.assertIsNone(shelf_reader.skip_reason(shelf, "test_b", "Fast", "test_x"))
        self.assertIsNotNone(shelf_reader.skip_reason(shelf, "test_c", "Slow", "test_one"))
        self.assertIsNone(shelf_reader.skip_reason(shelf, "test_c", "Slow", "test_two"))
        self.assertIsNotNone(shelf_reader.skip_reason(shelf, "test_d", None, "test_function"))
        self.assertIsNone(shelf_reader.skip_reason(shelf, "test_e", None, "test_function"))

    def test_the_reason_carries_the_entrys_text_date_and_id(self):
        text = shelf_reader.skip_reason(shelf_with(ENTRY), "test_x", None, "test_y")
        self.assertIn(ENTRY["reason"], text)
        self.assertIn(ENTRY["date"], text)
        self.assertIn(ENTRY["id"], text)

    @unittest.skipIf(conftest is None, "pytest is not installed")
    def test_the_hook_marks_a_shelved_item_skipped_and_leaves_the_others(self):
        shelved, other = mock.Mock(), mock.Mock()
        shelved.path, shelved.cls, shelved.originalname, shelved.name = "tools/tests/test_x.py", None, "test_a", "test_a"
        other.path, other.cls, other.originalname, other.name = "tools/tests/test_y.py", None, "test_a", "test_a"
        with mock.patch.object(shelf_reader, "load", return_value=shelf_with(ENTRY)):
            conftest.pytest_collection_modifyitems(None, [shelved, other])
        shelved.add_marker.assert_called_once()
        other.add_marker.assert_not_called()
        self.assertIn("shelved", shelved.add_marker.call_args.args[0].kwargs["reason"])

    def test_the_file_the_load_disable_covered_that_remains_shelved_is_skipped_through_the_shelf(self):
        shelf = shelf_reader.load()
        reason = shelf_reader.skip_reason(shelf, "test_package_release", "AnyClass", "test_any")
        self.assertIsNotNone(reason, "test_package_release")
        self.assertIn("shelved", reason)
        self.assertIn("pytest:test_package_release", reason)


class GTestFilterTest(unittest.TestCase):
    def test_the_filter_keeps_only_the_binarys_shelved_tests_out(self):
        shelf = shelf_with(entry("gtest:test_a#S.One"), entry("gtest:test_a#S.Two"), entry("gtest:test_b#S.Three"))
        self.assertEqual(shelf_reader.gtest_filter(shelf, "test_a"), "-S.One:S.Two")
        self.assertEqual(shelf_reader.gtest_filter(shelf, "test_c"), "")
        self.assertEqual(shelf_reader.gtest_filter(shelf, "test_c", "X.*"), "X.*")
        self.assertEqual(shelf_reader.gtest_filter(shelf, "test_b", "X.*"), "X.*-S.Three")
        self.assertEqual(shelf_reader.gtest_filter(shelf, "test_b", "X.*-Y.z"), "X.*-Y.z:S.Three")


@unittest.skipUnless(HAS_TIMEOUT_PLUGIN, "pytest-timeout is not installed")
class CeilingFlagsTest(unittest.TestCase):
    def test_the_runner_passes_the_shelfs_ceiling_to_pytest_timeout(self):
        flags, problem = run_tool_tests.timeout_flags(["-q", "tools/tests"], {})
        self.assertIsNone(problem)
        self.assertEqual(flags, ["-p", "timeout", f"--timeout={OWNER_CEILING_S}", "--timeout-method=thread"])

    def test_arguments_cannot_raise_or_switch_off_the_ceiling(self):
        for args in (["--timeout=999"], ["--timeout", "999"], ["-p", "no:timeout"], ["--timeout-method=signal"]):
            flags, problem = run_tool_tests.timeout_flags(args, {})
            self.assertEqual(flags, [], args)
            self.assertIn("ceiling", problem, args)
        flags, problem = run_tool_tests.timeout_flags([], {"PYTEST_ADDOPTS": "--timeout=999"})
        self.assertEqual(flags, [])
        self.assertIsNotNone(problem)

    def test_a_run_without_pytest_timeout_is_refused(self):
        with mock.patch.object(run_tool_tests.importlib.util, "find_spec", return_value=None):
            flags, problem = run_tool_tests.timeout_flags([], {})
        self.assertEqual(flags, [])
        self.assertIn("pytest-timeout is not installed", problem)

    def test_the_command_the_runner_starts_carries_the_ceiling(self):
        with tempfile.TemporaryDirectory() as logs:
            env = {**os.environ, "NEVR_TEST_LOG_DIR": logs}
            done = subprocess.run([sys.executable, str(TOOLS / "run_tool_tests.py"), "-q", "-p", "no:randomly",
                                   str(TOOLS / "tests" / "test_adr_numbers.py")], env=env, capture_output=True,
                                  text=True, timeout=RUN_BUDGET_S, check=False)
            records = [json.loads(line) for line in (pathlib.Path(logs) / "runs.jsonl").read_text().splitlines()]
        start = next(r for r in records if r.get("event") == "start")
        self.assertIn(f"--timeout={OWNER_CEILING_S}", start["cmd"], done.stdout + done.stderr)
        self.assertIn("--timeout-method=thread", start["cmd"])


@unittest.skipUnless(HAS_TIMEOUT_PLUGIN, "pytest-timeout is not installed")
class PytestTimeoutFailsATestOverTheCeilingTest(unittest.TestCase):
    def test_a_test_that_runs_past_the_ceiling_fails_and_one_under_it_passes(self):
        source = (f"import time\n\ndef test_over():\n    time.sleep({PROBE_SLEEP_S})\n\ndef test_under():\n    pass\n")
        with tempfile.TemporaryDirectory() as d:
            probe = pathlib.Path(d) / "test_probe.py"
            probe.write_text(source, encoding="utf-8")
            env = {k: v for k, v in os.environ.items() if k != "PYTEST_ADDOPTS"}
            env["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
            done = subprocess.run([sys.executable, "-m", "pytest", "-p", "timeout", f"--timeout={PROBE_CEILING_S}",
                                   "--timeout-method=thread", "-p", "no:cacheprovider", "-q", str(probe)], env=env, capture_output=True, text=True,
                                  timeout=PROBE_BUDGET_S, check=False, cwd=d)
        self.assertNotEqual(done.returncode, 0, done.stdout)
        self.assertIn("Timeout", done.stdout + done.stderr)


CMAKE_MODULE = REPO / "cmake" / "TestShelf.cmake"
HOST_TESTS = REPO / "src" / "quest" / "host_tests"
CMAKE_BUDGET_S = 60


def cmake_reads(shelf: dict):
    """(exit code, output) of cmake -P on a script that includes cmake/TestShelf.cmake over a shelf file made from `shelf`."""
    with tempfile.TemporaryDirectory() as d:
        shelf_file = pathlib.Path(d) / "shelf.json"
        shelf_file.write_text(json.dumps(shelf), encoding="utf-8")
        script = pathlib.Path(d) / "probe.cmake"
        script.write_text(f'set(NEVR_TEST_SHELF "{shelf_file}")\ninclude("{CMAKE_MODULE}")\n'
                          'message(STATUS "CEILING=${NEVR_TEST_CEILING_S} SHELVED=${NEVR_SHELVED_CTESTS}")\n',
                          encoding="utf-8")
        done = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=CMAKE_BUDGET_S,
                              check=False)
    return done.returncode, done.stdout + done.stderr


@unittest.skipUnless(shutil.which("cmake"), "cmake not installed")
class CMakeShelfTest(unittest.TestCase):
    def test_cmake_reads_the_ceiling_and_the_shelved_ctests(self):
        code, out = cmake_reads(shelf_with(entry("ctest:slow_one"), entry("pytest:test_x"), entry("ctest:slow_two"),
                                           ceiling=7))
        self.assertEqual(code, 0, out)
        self.assertIn("CEILING=7 SHELVED=slow_one;slow_two", out)

    def test_cmake_reads_an_empty_shelf(self):
        code, out = cmake_reads(shelf_with())
        self.assertEqual(code, 0, out)
        self.assertIn(f"CEILING={OWNER_CEILING_S} SHELVED=", out)

    def test_cmake_refuses_a_ceiling_that_is_not_a_whole_number(self):
        code, out = cmake_reads(shelf_with(ceiling="30s"))
        self.assertNotEqual(code, 0, out)
        self.assertIn("ceiling_s", out)

    def test_the_real_shelf_gives_cmake_the_python_readers_ceiling(self):
        script = f'include("{CMAKE_MODULE}")\nmessage(STATUS "CEILING=${{NEVR_TEST_CEILING_S}}")\n'
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / "probe.cmake"
            path.write_text(script, encoding="utf-8")
            done = subprocess.run(["cmake", "-P", str(path)], capture_output=True, text=True, timeout=CMAKE_BUDGET_S,
                                  check=False)
        self.assertIn(f"CEILING={shelf_reader.ceiling_s()}", done.stdout + done.stderr)

    def test_a_quest_ctest_test_takes_the_ceiling_and_no_limit_of_its_own(self):
        # These are the lines cmake reads when it registers a test: the one registration gives every test the ceiling and
        # refuses a TIMEOUT argument, and no group file passes one.
        register = (HOST_TESTS / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("TIMEOUT ${NEVR_TEST_CEILING_S}", register)
        self.assertIn("if(DEFINED A_TIMEOUT)", register)
        self.assertIn("DISABLED TRUE", register)
        own = [f"{p.name}:{i}" for p in sorted((HOST_TESTS / "groups").glob("*.cmake"))
               for i, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1)
               if re.search(r"\bTIMEOUT\s+[0-9]", line)]
        self.assertEqual(own, [], "a test passes its own TIMEOUT: the ceiling is never raised for a test")


UNITTEST_RUNNER = TOOLS / "unittest_ceiling.py"
UNITTEST_BUDGET_S = 60
UNITTEST_CEILING_S = 1
UNITTEST_SOURCE = f"""import time
import unittest


class Cases(unittest.TestCase):
    def test_fast(self):
        pass

    def test_shelved(self):
        self.fail("a shelved test ran")

    def test_slow(self):
        time.sleep({PROBE_SLEEP_S})
"""


def unittest_run(shelf: dict, source: str = UNITTEST_SOURCE):
    with tempfile.TemporaryDirectory() as d:
        suite = pathlib.Path(d) / "suite"
        suite.mkdir()
        (suite / "test_cases.py").write_text(source, encoding="utf-8")
        shelf_file = pathlib.Path(d) / "shelf.json"
        shelf_file.write_text(json.dumps(shelf), encoding="utf-8")
        done = subprocess.run([sys.executable, str(UNITTEST_RUNNER), "--shelf", str(shelf_file), "discover", "-s",
                               str(suite), "-p", "test_*.py"], capture_output=True, text=True,
                              timeout=UNITTEST_BUDGET_S, check=False, cwd=d)
    return done


class UnittestCeilingTest(unittest.TestCase):
    def test_a_test_over_the_ceiling_is_named_and_stops_the_run_with_124(self):
        done = unittest_run(shelf_with(entry("pytest:test_cases#Cases.test_shelved"), ceiling=UNITTEST_CEILING_S))
        self.assertEqual(done.returncode, 124, done.stdout + done.stderr)
        self.assertIn("test_cases.Cases.test_slow ran longer than 1 s", done.stderr)

    def test_a_shelved_test_is_skipped_with_its_reason_and_the_rest_pass(self):
        fast_only = UNITTEST_SOURCE.replace(f"time.sleep({PROBE_SLEEP_S})", "pass")
        done = unittest_run(shelf_with(entry("pytest:test_cases#Cases.test_shelved")), fast_only)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("skipped", done.stderr)
        self.assertIn("1 test(s) shelved", done.stderr)

    def test_without_the_shelf_entry_the_shelved_test_would_run_and_fail(self):
        fast_only = UNITTEST_SOURCE.replace(f"time.sleep({PROBE_SLEEP_S})", "pass")
        done = unittest_run(shelf_with(), fast_only)
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn("a shelved test ran", done.stderr)

    def test_a_broken_shelf_is_a_usage_error(self):
        done = unittest_run(shelf_with(ceiling=0))
        self.assertEqual(done.returncode, 2, done.stderr)

    def test_the_verify_recipe_runs_the_python_suite_through_this_runner(self):
        line = next(ln for ln in (REPO / "justfile").read_text(encoding="utf-8").splitlines()
                    if "unittest_ceiling.py" in ln)
        self.assertIn("discover -s tools/tests", line)


if __name__ == "__main__":
    unittest.main()
