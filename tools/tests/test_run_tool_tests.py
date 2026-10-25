"""tools/run_tool_tests.py: every tools/tests run leaves a record that names its failures (#913).

These tests run the real script, as `just test-tools` does, against small suites written for each case.
"""

import datetime
import importlib.util
import json
import os
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "run_tool_tests.py"
JUSTFILE = REPO / "justfile"

SUITE = '''
def test_holds():
    assert True

def test_breaks():
    assert 1 == 2
'''

ERROR_SUITE = '''
import pytest

@pytest.fixture
def broken():
    raise RuntimeError("fixture setup fails")

def test_uses_broken(broken):
    pass
'''

SLOW_SUITE = '''
import os, pathlib, time

def test_waits():
    # pytest's own process group, then its parent's (the wrapper's).
    groups = f"{os.getpgid(0)} {os.getpgid(os.getppid())}"
    pathlib.Path(os.environ["RUN_TOOL_TESTS_MARKER"]).write_text(groups)
    time.sleep(60)
'''

# Overwrites the JUnit XML after pytest's junitxml plugin wrote it (sessionfinish), before pytest exits.
GARBLE_CONFTEST = '''
def pytest_unconfigure(config):
    path = config.option.xmlpath
    if path:
        with open(path, "w") as f:
            f.write("<not xml")
'''


def load_script():
    spec = importlib.util.spec_from_file_location("run_tool_tests", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RunToolTestsRecord(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="run-tool-tests-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.suite = self.write_suite("suite", SUITE)
        self.logs = self.root / "logs"

    def write_suite(self, name: str, body: str, conftest: str = "") -> Path:
        d = self.root / name
        d.mkdir(parents=True)
        (d / "test_sample.py").write_text(body, encoding="utf-8")
        if conftest:
            (d / "conftest.py").write_text(conftest, encoding="utf-8")
        return d

    def env(self, log_dir: Path) -> dict:
        return dict(os.environ, NEVR_TEST_LOG_DIR=str(log_dir))

    def run_script(self, log_dir: Path, *args: str, cwd: Path = REPO) -> subprocess.CompletedProcess:
        argv = list(args) if args else ["-q", "-p", "no:cacheprovider", str(self.suite)]
        return subprocess.run([sys.executable, str(SCRIPT), *argv], capture_output=True, text=True,
                              cwd=cwd, env=self.env(log_dir), timeout=120)

    def records(self, event: str = "end") -> list:
        lines = (self.logs / "runs.jsonl").read_text().splitlines()
        return [r for r in map(json.loads, lines) if r["event"] == event]

    def test_a_failing_run_keeps_its_output_names_the_failure_and_keeps_pytests_exit_code(self):
        result = self.run_script(self.logs)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

        records = self.records()
        self.assertEqual(len(records), 1)
        record = records[0]
        self.assertEqual(record["rc"], 1)
        self.assertEqual(record["failed"], ["test_sample::test_breaks"])
        self.assertEqual(record["junit_state"], "ok")
        self.assertFalse(record["interrupted"])
        self.assertIn("1 failed, 1 passed", record["summary"])
        self.assertIn(record["tree"], ("clean", "DIRTY"))
        self.assertRegex(record["head"], r"^[0-9a-f]{40}$")
        self.assertRegex(record["pytest"], r"^\d+\.")

        log = Path(record["log"]).read_text()
        self.assertEqual(Path(record["log"]).parent, self.logs)
        self.assertIn(f"test-tools: HEAD {record['head']}", log)
        self.assertIn(f"test-tools: cmd {' '.join(record['cmd'])}", log)
        self.assertIn("FAILED", log)
        self.assertIn("test_breaks", log)
        self.assertRegex(log, r"\ntest-tools: end \S+ rc=1\n$")

        cases = {c.get("name"): c for c in ET.parse(record["junit"]).getroot().iter("testcase")}
        self.assertIsNotNone(cases["test_breaks"].find("failure"))
        self.assertIsNone(cases["test_holds"].find("failure"))
        self.assertIn(f"test-tools: log {record['log']}", result.stdout)

    def test_pytests_output_is_echoed_to_stdout(self):
        result = self.run_script(self.logs)
        self.assertIn("FAILED", result.stdout)
        self.assertIn("1 failed, 1 passed", result.stdout)
        self.assertIn("test-tools: end ", result.stdout)

    def test_output_that_is_not_utf8_is_kept_not_a_crash(self):
        suite = self.write_suite("bytes", "import sys\n\ndef test_prints():\n"
                                 "    sys.stdout.buffer.write(b'raw \\xff byte\\n')\n    sys.stdout.flush()\n")
        result = self.run_script(self.logs, "-q", "-s", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.records()[0]["rc"], 0)
        self.assertIn("raw \ufffd byte", Path(self.records()[0]["log"]).read_text())

    def test_a_start_record_is_written_before_pytest_runs(self):
        self.run_script(self.logs)
        starts, ends = self.records("start"), self.records("end")
        self.assertEqual(len(starts), 1)
        self.assertEqual(starts[0]["log"], ends[0]["log"])
        self.assertNotIn("rc", starts[0])

    def test_each_run_appends_its_own_record(self):
        self.run_script(self.logs)
        self.run_script(self.logs)
        records = self.records()
        self.assertEqual(len(records), 2)
        self.assertEqual(len({r["junit"] for r in records} | {r["log"] for r in records}), 4)

    def test_two_runs_in_the_same_instant_get_different_files(self):
        module = load_script()
        now = datetime.datetime.now(datetime.timezone.utc)
        self.assertNotEqual(module.run_stem(now, "a" * 40), module.run_stem(now, "a" * 40))

    def test_an_errored_test_is_named_like_a_failed_one(self):
        suite = self.write_suite("errors", ERROR_SUITE)
        result = self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.records()[0]["failed"], ["test_sample::test_uses_broken"])

    def test_an_unparseable_junit_file_is_recorded_not_a_crash(self):
        suite = self.write_suite("garbled", SUITE, GARBLE_CONFTEST)
        result = self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual(record["junit_state"], "unparseable")
        self.assertEqual(record["failed"], [])

    def test_a_run_with_no_tests_keeps_its_summary(self):
        empty = self.root / "empty"
        empty.mkdir()
        result = self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(empty))
        self.assertEqual(result.returncode, 5, result.stdout + result.stderr)
        self.assertIn("no tests ran", self.records()[0]["summary"])

    def test_with_no_arguments_it_runs_tools_tests_quietly(self):
        repo = self.root / "repo"
        (repo / "tools" / "tests").mkdir(parents=True)
        (repo / "tools" / "tests" / "test_sample.py").write_text(SUITE, encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True, cwd=repo,
                                env=self.env(self.logs), timeout=120)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual(record["cmd"][-2:], ["-q", "tools/tests"])
        self.assertEqual(record["failed"], ["tools.tests.test_sample::test_breaks"])

    def test_a_passed_junitxml_is_refused(self):
        result = self.run_script(self.logs, "--junitxml=elsewhere.xml", str(self.suite))
        self.assertEqual(result.returncode, 2)
        self.assertIn("test-tools: FAIL --junitxml is not accepted", result.stderr)
        self.assertFalse((self.logs / "runs.jsonl").exists())

    @unittest.skipIf(importlib.util.find_spec("pytest_randomly") is None, "pytest-randomly is not installed")
    def test_the_randomly_seed_is_drawn_passed_and_recorded(self):
        self.run_script(self.logs)
        record = self.records()[0]
        self.assertIsInstance(record["randomly_seed"], int)
        self.assertIn(f"--randomly-seed={record['randomly_seed']}", record["cmd"])
        self.assertIn(f"test-tools: randomly-seed {record['randomly_seed']}", Path(record["log"]).read_text())

    def test_a_seed_in_the_arguments_is_left_alone(self):
        self.run_script(self.logs, "-q", "-p", "no:cacheprovider", "-p", "no:randomly", str(self.suite))
        record = self.records()[0]
        self.assertIsNone(record["randomly_seed"])
        self.assertFalse(any(a.startswith("--randomly-seed") for a in record["cmd"]))

    def test_an_unwritable_log_directory_refuses_the_run(self):
        blocker = self.root / "a-file"
        blocker.write_text("")
        result = self.run_script(blocker / "logs")
        self.assertEqual(result.returncode, 2)
        self.assertIn("test-tools: FAIL cannot write the run record", result.stderr)
        self.assertNotIn("passed", result.stdout)

    @unittest.skipIf(os.geteuid() == 0, "root writes into a read-only directory")
    def test_a_read_only_log_directory_refuses_the_run(self):
        self.logs.mkdir()
        self.logs.chmod(stat.S_IRUSR | stat.S_IXUSR)
        self.addCleanup(self.logs.chmod, stat.S_IRWXU)
        result = self.run_script(self.logs)
        self.assertEqual(result.returncode, 2)
        self.assertIn("test-tools: FAIL cannot write the run record", result.stderr)
        self.assertEqual(list(self.logs.iterdir()), [])

    def test_an_end_record_that_cannot_be_appended_is_loud_and_never_green(self):
        if os.geteuid() == 0:
            self.skipTest("root appends to a read-only file")
        body = SLOW_SUITE.replace("time.sleep(60)", "time.sleep(1)")
        passing = self.write_suite("passing", body)
        marker = self.root / "marker"
        env = dict(self.env(self.logs), RUN_TOOL_TESTS_MARKER=str(marker))
        argv = [sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(passing)]
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                cwd=REPO, env=env)
        # The test is running, so the start record is in; the end record finds the index read-only.
        deadline = time.monotonic() + 60
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        index = self.logs / "runs.jsonl"
        index.chmod(stat.S_IRUSR)
        self.addCleanup(index.chmod, stat.S_IRUSR | stat.S_IWUSR)
        stdout, stderr = proc.communicate(timeout=120)
        self.assertIn("1 passed", stdout)
        self.assertEqual(proc.returncode, 2, stdout + stderr)
        self.assertIn("test-tools: FAIL cannot append the end record", stderr)
        self.assertNotIn("test-tools: log ", stdout)

    def test_an_interrupt_is_passed_on_and_still_recorded(self):
        slow = self.write_suite("slow", SLOW_SUITE)
        marker = self.root / "marker"
        env = dict(self.env(self.logs), RUN_TOOL_TESTS_MARKER=str(marker))
        argv = [sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(slow)]
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                cwd=REPO, env=env)
        deadline = time.monotonic() + 60
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(marker.exists(), "the slow test never started")
        # A terminal's CTRL+C goes to the wrapper's process group; pytest must not be in it.
        pytest_group, wrapper_group = marker.read_text().split()
        self.assertNotEqual(pytest_group, wrapper_group)
        # The log is on disk while pytest still runs, not only when it exits.
        live_log = Path(self.records("start")[0]["log"]).read_text()
        self.assertIn("test-tools: cmd ", live_log)
        started = time.monotonic()
        proc.send_signal(signal.SIGINT)
        stdout, stderr = proc.communicate(timeout=60)
        self.assertLess(time.monotonic() - started, 30, "pytest was not interrupted")
        self.assertEqual(proc.returncode, 130, stdout + stderr)
        record = self.records()[0]
        self.assertTrue(record["interrupted"])
        self.assertEqual(record["rc"], 130)
        self.assertIsNotNone(record["pytest_rc"])
        self.assertIn("interrupted=SIGINT", Path(record["log"]).read_text())

    def test_the_record_says_whether_the_tree_was_clean(self):
        repo = self.root / "repo"
        repo.mkdir()
        git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
        subprocess.run([*git, "init", "-q"], check=True)
        (repo / "f").write_text("x")
        subprocess.run([*git, "add", "f"], check=True)
        subprocess.run([*git, "commit", "-q", "-m", "c"], check=True)
        self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(self.suite), cwd=repo)
        (repo / "untracked").write_text("y")
        self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(self.suite), cwd=repo)
        self.assertEqual([r["tree"] for r in self.records()], ["clean", "DIRTY"])

    @unittest.skipIf(shutil.which("just") is None, "just is not installed")
    def test_the_recipe_passes_each_argument_through_whole(self):
        spaced = self.root / "dir with spaces"
        shutil.copytree(self.suite, spaced)
        result = subprocess.run(["just", "--justfile", str(JUSTFILE), "--working-directory", str(REPO),
                                 "test-tools", "-q", "-p", "no:cacheprovider", str(spaced)],
                                capture_output=True, text=True, env=self.env(self.logs), timeout=120)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual(record["cmd"][-1], str(spaced))
        self.assertEqual(record["failed"], ["test_sample::test_breaks"])


if __name__ == "__main__":
    unittest.main()
