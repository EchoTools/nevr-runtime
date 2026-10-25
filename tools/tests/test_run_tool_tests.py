"""tools/run_tool_tests.py: every tools/tests run leaves a record that names its failures (#913).

These tests run the real script, as `just test-tools` does, against small suites written for each case.
"""

import datetime
import importlib.metadata
import importlib.util
import io
import json
import os
import resource
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
import types
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
    # pytest's own process group, then its parent's (the wrapper's), written whole (rename) so the
    # test never reads it half-written.
    marker = pathlib.Path(os.environ["RUN_TOOL_TESTS_MARKER"])
    tmp = marker.with_suffix(".tmp")
    tmp.write_text(f"{os.getpgid(0)} {os.getpgid(os.getppid())}")
    tmp.rename(marker)
    go = os.environ.get("RUN_TOOL_TESTS_GO")
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not (go and pathlib.Path(go).exists()):
        time.sleep(0.05)
'''

# Ignores the first SIGINT, so only a second signal can stop it.
STUBBORN_SUITE = SLOW_SUITE.replace("import os, pathlib, time", "import os, pathlib, signal, time").replace(
    "    marker = ", "    signal.signal(signal.SIGINT, signal.SIG_IGN)\n    marker = ")

# Exits at once but leaves a process holding pytest's output open (run with -s).
STRAY_SUITE = '''
import subprocess

def test_leaves_a_child():
    subprocess.Popen(["sleep", "300"])
'''

XFAIL_SUITE = '''
import pytest

@pytest.mark.xfail(strict=True)
def test_known():
    assert False
'''

# Deletes the JUnit XML after pytest's junitxml plugin wrote it.
DELETE_CONFTEST = '''
import os

def pytest_unconfigure(config):
    path = config.option.xmlpath
    if path and os.path.exists(path):
        os.remove(path)
'''

# Overwrites the JUnit XML after pytest's junitxml plugin wrote it (sessionfinish), before pytest exits.
GARBLE_CONFTEST = '''
def pytest_unconfigure(config):
    path = config.option.xmlpath
    if path:
        with open(path, "w") as f:
            f.write("<not xml")
'''


PASS_SUITE = '''
def test_holds():
    assert True
'''

# Exits at once but leaves a process in a session of its own holding pytest's output open (run with
# -s): a kill of pytest's process group does not reach it. Its pid goes to $RUN_TOOL_TESTS_MARKER.
SETSID_STRAY_SUITE = '''
import os, subprocess

def test_leaves_a_child_in_its_own_session():
    child = subprocess.Popen(["sleep", "300"], start_new_session=True)
    with open(os.environ["RUN_TOOL_TESTS_MARKER"], "w") as f:
        f.write(str(child.pid))
'''

# Runs the script's main() with a change made first ({prelude}), as `python3 tools/run_tool_tests.py` would.
DRIVER = '''
import importlib.util, os, signal, subprocess, sys
spec = importlib.util.spec_from_file_location("run_tool_tests", {script!r})
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
{prelude}
sys.exit(m.main(sys.argv[1:]))
'''


def kill_group(pgid: int) -> None:
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def load_script():
    spec = importlib.util.spec_from_file_location("run_tool_tests", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RunToolTestsRecord(unittest.TestCase):
    def setUp(self):
        # An ignored signal survives exec, and the script keeps it ignored (as under nohup); a shell's
        # background job starts with SIGINT ignored. The script under test starts with the defaults.
        for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            if signal.getsignal(s) is signal.SIG_IGN:
                signal.signal(s, signal.SIG_DFL)
                self.addCleanup(signal.signal, s, signal.SIG_IGN)
        self.root = Path(tempfile.mkdtemp(prefix="run-tool-tests-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.suite = self.write_suite("suite", SUITE)
        self.logs = self.root / "logs"

    def write_suite(self, name: str, body: str, conftest: str = "") -> Path:
        d = self.root / name
        d.mkdir(parents=True)
        (d / "test_sample.py").write_text(body, encoding="utf-8")
        # Pins pytest's rootdir to the suite, so test ids do not depend on where the temp dir sits.
        (d / "pytest.ini").write_text("[pytest]\n", encoding="utf-8")
        if conftest:
            (d / "conftest.py").write_text(conftest, encoding="utf-8")
        return d

    def env(self, log_dir: Path, **extra: str) -> dict:
        env = dict(os.environ, NEVR_TEST_LOG_DIR=str(log_dir), **extra)
        env.pop("PYTEST_ADDOPTS", None)
        env.update({k: v for k, v in extra.items() if k == "PYTEST_ADDOPTS"})
        return env

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
        self.assertEqual(record["pytest"], importlib.metadata.version("pytest"))
        self.assertTrue(record["output_complete"])
        self.assertFalse(record["stray_kill_sent"])
        self.assertIsNone(record["read_error"])
        self.assertIsNone(record["log_error"])
        self.assertEqual(record["signals"], [])

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

    def test_the_installed_pytest_plugins_are_recorded(self):
        self.run_script(self.logs)
        record = self.records()[0]
        expected = {ep.name for ep in importlib.metadata.entry_points(group="pytest11")}
        self.assertEqual(set(record["plugins"]), expected)
        for name, dist in record["plugins"].items():
            self.assertRegex(dist, r"\S+ \S+")
        self.assertIn("test-tools: plugins ", Path(record["log"]).read_text())

    def test_an_xfail_run_keeps_its_summary(self):
        suite = self.write_suite("xfail", XFAIL_SUITE)
        result = self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 xfailed", self.records()[0]["summary"])

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

    def test_a_missing_junit_file_is_recorded(self):
        suite = self.write_suite("deleted", SUITE, DELETE_CONFTEST)
        result = self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual(record["junit_state"], "missing")
        self.assertEqual(record["failed"], [])

    def test_a_run_with_no_tests_keeps_its_summary(self):
        empty = self.root / "empty"
        empty.mkdir()
        (empty / "pytest.ini").write_text("[pytest]\n", encoding="utf-8")
        result = self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(empty))
        self.assertEqual(result.returncode, 5, result.stdout + result.stderr)
        self.assertIn("no tests ran", self.records()[0]["summary"])

    def test_with_no_arguments_it_runs_tools_tests_quietly(self):
        repo = self.root / "repo"
        (repo / "tools" / "tests").mkdir(parents=True)
        (repo / "tools" / "tests" / "test_sample.py").write_text(SUITE, encoding="utf-8")
        (repo / "pytest.ini").write_text("[pytest]\n", encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True, cwd=repo,
                                env=self.env(self.logs), timeout=120)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual(record["cmd"][-2:], ["-q", "tools/tests"])
        self.assertEqual(record["failed"], ["tools.tests.test_sample::test_breaks"])

    def test_a_passed_junitxml_is_refused(self):
        elsewhere = self.root / "elsewhere.xml"
        for spelling in (["--junitxml", str(elsewhere)], [f"--junitxml={elsewhere}"],
                         ["--junit-xml", str(elsewhere)], [f"--junit-xml={elsewhere}"]):
            with self.subTest(spelling=spelling[0]):
                result = self.run_script(self.logs, *spelling, str(self.suite))
                self.assertEqual(result.returncode, 2)
                self.assertIn("test-tools: FAIL --junitxml is not accepted", result.stderr)
                self.assertFalse((self.logs / "runs.jsonl").exists())
                self.assertFalse(elsewhere.exists())

    @unittest.skipIf(importlib.util.find_spec("pytest_randomly") is None, "pytest-randomly is not installed")
    def test_the_randomly_seed_is_drawn_passed_and_recorded(self):
        self.run_script(self.logs)
        record = self.records()[0]
        self.assertIsInstance(record["randomly_seed"], int)
        self.assertIn(f"--randomly-seed={record['randomly_seed']}", record["cmd"])
        self.assertIn(f"test-tools: randomly-seed {record['randomly_seed']}", Path(record["log"]).read_text())

    def test_randomly_disabled_in_the_arguments_adds_no_seed(self):
        self.run_script(self.logs, "-q", "-p", "no:cacheprovider", "-p", "no:randomly", str(self.suite))
        record = self.records()[0]
        self.assertIsNone(record["randomly_seed"])
        self.assertFalse(any(a.startswith("--randomly-seed") for a in record["cmd"]))

    @unittest.skipIf(importlib.util.find_spec("pytest_randomly") is None, "pytest-randomly is not installed")
    def test_randomly_disabled_in_pytest_addopts_adds_no_seed_and_the_run_still_runs(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(self.suite)],
                                capture_output=True, text=True, cwd=REPO, timeout=120,
                                env=self.env(self.logs, PYTEST_ADDOPTS="-p no:randomly"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertIsNone(record["randomly_seed"])
        self.assertFalse(any(a.startswith("--randomly-seed") for a in record["cmd"]))
        self.assertEqual(record["failed"], ["test_sample::test_breaks"])

    @unittest.skipIf(importlib.util.find_spec("pytest_randomly") is None, "pytest-randomly is not installed")
    def test_a_given_seed_is_recorded_as_given(self):
        self.run_script(self.logs, "-q", "-p", "no:cacheprovider", "--randomly-seed=5", str(self.suite))
        record = self.records()[0]
        self.assertEqual(record["randomly_seed"], 5)
        self.assertEqual([a for a in record["cmd"] if a.startswith("--randomly-seed")], ["--randomly-seed=5"])

    @unittest.skipIf(importlib.util.find_spec("pytest_randomly") is None, "pytest-randomly is not installed")
    def test_a_path_that_says_randomly_still_gets_a_seed(self):
        named = self.root / "randomly_dir"
        shutil.copytree(self.suite, named)
        self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(named))
        self.assertIsInstance(self.records()[0]["randomly_seed"], int)

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

    @unittest.skipIf(os.geteuid() == 0, "root appends to a read-only file")
    def test_a_start_record_that_cannot_be_appended_refuses_the_run(self):
        self.logs.mkdir()
        index = self.logs / "runs.jsonl"
        index.write_text("")
        index.chmod(stat.S_IRUSR)
        self.addCleanup(index.chmod, stat.S_IRUSR | stat.S_IWUSR)
        result = self.run_script(self.logs)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("test-tools: FAIL cannot append the start record", result.stderr)
        self.assertNotIn("passed", result.stdout)
        self.assertEqual(sorted(p.name for p in self.logs.iterdir()), ["runs.jsonl"])

    def start_slow(self, body: str = SLOW_SUITE, *extra_args: str, preexec_fn=None, **extra_env: str):
        """Start the wrapper in its own session on a suite that waits; return it once the test runs."""
        slow = self.write_suite("slow", body)
        self.marker = self.root / "marker"
        self.go = self.root / "go"
        env = self.env(self.logs, RUN_TOOL_TESTS_MARKER=str(self.marker), RUN_TOOL_TESTS_GO=str(self.go),
                       **extra_env)
        argv = [sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", *extra_args, str(slow)]
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                cwd=REPO, env=env, start_new_session=True, preexec_fn=preexec_fn)
        self.addCleanup(lambda: proc.poll() is None and os.killpg(proc.pid, signal.SIGKILL))
        deadline = time.monotonic() + 60
        while not self.marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(self.marker.exists(), "the slow test never started")
        self.addCleanup(kill_group, int(self.marker.read_text().split()[0]))  # pytest's own session
        return proc

    @unittest.skipIf(os.geteuid() == 0, "root appends to a read-only file")
    def test_an_end_record_that_cannot_be_appended_is_loud_and_never_green(self):
        proc = self.start_slow()
        # The test is running, so the start record is in; the end record finds the index read-only.
        index = self.logs / "runs.jsonl"
        index.chmod(stat.S_IRUSR)
        self.addCleanup(index.chmod, stat.S_IRUSR | stat.S_IWUSR)
        self.go.write_text("")
        stdout, stderr = proc.communicate(timeout=120)
        self.assertIn("1 passed", stdout)
        self.assertEqual(proc.returncode, 2, stdout + stderr)
        self.assertIn("test-tools: FAIL cannot append the end record", stderr)
        self.assertNotIn("test-tools: log ", stdout)

    def read_stdout_until(self, proc, text: str, timeout: float = 30) -> str:
        """The wrapper's stdout up to the line containing `text`, read while it still runs."""
        seen = []

        def read():
            for line in iter(proc.stdout.readline, ""):
                seen.append(line)
                if text in line:
                    return

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        reader.join(timeout)
        return "".join(seen)

    def assert_interrupted(self, proc, sent: str, rc: int, signals: list) -> dict:
        started = time.monotonic()
        stdout, stderr = proc.communicate(timeout=90)
        self.assertLess(time.monotonic() - started, 25, "pytest was not stopped")
        self.assertEqual(proc.returncode, rc, stdout + stderr)
        self.assertNotIn("Traceback", stderr)
        record = self.records()[0]
        self.assertTrue(record["interrupted"])
        self.assertEqual(record["signals"], signals)
        self.assertEqual(record["rc"], rc)
        self.assertIsNotNone(record["pytest_rc"])
        self.assertIn(f"interrupted={sent}", Path(record["log"]).read_text())
        return record

    def test_an_interrupt_is_passed_on_and_still_recorded(self):
        proc = self.start_slow()
        # A terminal's CTRL+C goes to the wrapper's process group; pytest must not be in it.
        pytest_group, wrapper_group = self.marker.read_text().split()
        self.assertNotEqual(pytest_group, wrapper_group)
        # The log is on disk, and the header on stdout, while pytest still runs.
        live_log = Path(self.records("start")[0]["log"]).read_text()
        self.assertIn("test-tools: cmd ", live_log)
        self.assertIn("test-tools: cmd ", self.read_stdout_until(proc, "test-tools: cmd "))
        os.killpg(proc.pid, signal.SIGINT)
        record = self.assert_interrupted(proc, "SIGINT", 130, ["SIGINT"])
        # pytest got one SIGINT and wrote its own summary, which the log keeps.
        self.assertIn("KeyboardInterrupt", Path(record["log"]).read_text())

    def test_a_terminate_is_passed_on_as_an_interrupt_and_recorded(self):
        proc = self.start_slow()
        proc.send_signal(signal.SIGTERM)
        record = self.assert_interrupted(proc, "SIGTERM", 128 + signal.SIGTERM, ["SIGTERM"])
        self.assertIn("KeyboardInterrupt", Path(record["log"]).read_text())

    def test_a_hangup_is_passed_on_and_recorded(self):
        proc = self.start_slow()
        os.killpg(proc.pid, signal.SIGHUP)
        record = self.assert_interrupted(proc, "SIGHUP", 128 + signal.SIGHUP, ["SIGHUP"])
        self.assertIn("KeyboardInterrupt", Path(record["log"]).read_text())

    def test_a_second_signal_kills_pytest_and_is_still_recorded(self):
        proc = self.start_slow(STUBBORN_SUITE)
        os.killpg(proc.pid, signal.SIGINT)
        time.sleep(1.5)  # further apart than a repeat of the same signal
        self.assertIsNone(proc.poll(), "the stubborn test stopped on the first SIGINT")
        os.killpg(proc.pid, signal.SIGINT)
        record = self.assert_interrupted(proc, "SIGINT", 130, ["SIGINT", "SIGINT"])
        self.assertEqual(record["pytest_rc"], -signal.SIGKILL)

    def test_pytest_is_killed_when_it_outlives_the_grace_period(self):
        proc = self.start_slow(STUBBORN_SUITE, NEVR_TEST_INTERRUPT_GRACE_S="2")
        os.killpg(proc.pid, signal.SIGINT)
        record = self.assert_interrupted(proc, "SIGINT", 130, ["SIGINT"])
        self.assertEqual(record["pytest_rc"], -signal.SIGKILL)

    def test_a_closed_stdout_loses_the_echo_not_the_record(self):
        proc = self.start_slow()
        proc.stdout.close()
        self.go.write_text("")
        _, stderr = proc.communicate(timeout=120)
        self.assertEqual(proc.returncode, 0, stderr)
        self.assertNotIn("Traceback", stderr)
        record = self.records()[0]
        self.assertEqual(record["rc"], 0)
        self.assertIn("1 passed", Path(record["log"]).read_text())
        self.assertRegex(Path(record["log"]).read_text(), r"\ntest-tools: end \S+ rc=0\n$")

    def test_a_process_left_holding_the_output_is_killed_and_recorded(self):
        stray = self.write_suite("stray", STRAY_SUITE)
        started = time.monotonic()
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-s", "-p", "no:cacheprovider", str(stray)],
                                capture_output=True, text=True, cwd=REPO, timeout=120,
                                env=self.env(self.logs, NEVR_TEST_DRAIN_S="1"))
        # $NEVR_TEST_DRAIN_S is read: the default 10 s would take the run past this.
        self.assertLess(time.monotonic() - started, 8)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertTrue(record["stray_kill_sent"])
        self.assertTrue(record["output_complete"])
        self.assertIn("1 passed", record["summary"])

    def test_output_written_shortly_after_pytest_exits_is_kept(self):
        # A child still writing for a moment after pytest exits is waited for, not killed.
        late = self.write_suite("late", "import subprocess\n\ndef test_leaves_a_writer():\n"
                                "    subprocess.Popen(['sh', '-c', 'sleep 1; echo LATE-LINE'])\n")
        result = self.run_script(self.logs, "-q", "-s", "-p", "no:cacheprovider", str(late))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertFalse(record["stray_kill_sent"])
        self.assertIn("LATE-LINE", Path(record["log"]).read_text())

    def test_a_log_that_cannot_be_written_is_loud_and_never_green(self):
        # A file-size limit makes the log's writes fail (Python ignores SIGXFSZ, so they raise) while
        # the small records and the XML still fit. pytest's output goes to a pipe, which the limit spares.
        loud = self.write_suite("loud", "def test_prints():\n    print('x' * 200_000)\n")
        limit = 64 * 1024
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-s", "-p", "no:cacheprovider", str(loud)],
                                capture_output=True, text=True, cwd=REPO, env=self.env(self.logs), timeout=120,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit)))
        self.assertEqual(result.returncode, 2, result.stdout[-2000:] + result.stderr)
        self.assertIn("test-tools: FAIL cannot write the log", result.stderr)
        record = self.records()[0]
        self.assertEqual(record["pytest_rc"], 0)
        self.assertIsNotNone(record["log_error"])

    def test_a_signal_before_pytest_starts_means_pytest_never_starts(self):
        # The wrapper's stdout is a pipe already full, so its first header line blocks: the start
        # record is written and pytest is not yet started. The signal lands there.
        read_end, write_end = os.pipe()
        os.set_blocking(write_end, False)
        try:
            while True:
                os.write(write_end, b"x" * 4096)
        except BlockingIOError:
            pass
        os.set_blocking(write_end, True)
        marker = self.root / "marker"
        env = self.env(self.logs, RUN_TOOL_TESTS_MARKER=str(marker), RUN_TOOL_TESTS_GO=str(self.root / "go"))
        slow = self.write_suite("slow", SLOW_SUITE)
        proc = subprocess.Popen([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(slow)],
                                stdout=write_end, stderr=subprocess.PIPE, text=True, cwd=REPO, env=env,
                                start_new_session=True)
        os.close(write_end)
        self.addCleanup(lambda: proc.poll() is None and os.killpg(proc.pid, signal.SIGKILL))
        index = self.logs / "runs.jsonl"
        deadline = time.monotonic() + 60
        while not (index.exists() and index.read_text()) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(index.exists(), "the start record never appeared")
        os.killpg(proc.pid, signal.SIGINT)
        time.sleep(0.5)
        drain = threading.Thread(target=lambda: [None for _ in iter(lambda: os.read(read_end, 65536), b"")],
                                 daemon=True)
        drain.start()
        proc.wait(timeout=60)
        self.assertEqual(proc.returncode, 130, proc.stderr.read())
        record = self.records()[0]
        self.assertEqual(record["signals"], ["SIGINT"])
        self.assertIsNone(record["pytest_rc"])
        self.assertFalse(marker.exists(), "pytest ran after the signal")
        self.assertIn("interrupted=SIGINT", Path(record["log"]).read_text())

    def run_driver(self, prelude: str, *args: str, preexec_fn=None, **extra_env: str):
        """Run main() with `prelude` applied to the loaded script first."""
        driver = self.root / "driver.py"
        driver.write_text(DRIVER.format(script=str(SCRIPT), prelude=prelude), encoding="utf-8")
        argv = list(args) if args else ["-q", "-p", "no:cacheprovider", str(self.suite)]
        return subprocess.run([sys.executable, str(driver), *argv], capture_output=True, text=True, cwd=REPO,
                              env=self.env(self.logs, **extra_env), timeout=120, preexec_fn=preexec_fn)

    def test_output_stdout_cannot_encode_is_replaced_in_the_echo_and_kept_in_the_log(self):
        # pytest writes UTF-8 bytes; only the script's own stdout is ASCII.
        suite = self.write_suite("accents", "import sys\n\ndef test_prints():\n"
                                 "    sys.stdout.buffer.write('caf\\u00e9 done\\n'.encode())\n"
                                 "    sys.stdout.flush()\n")
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-s", "-p", "no:cacheprovider", str(suite)],
                                capture_output=True, cwd=REPO, timeout=120,
                                env=self.env(self.logs, PYTHONIOENCODING="ascii"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b"caf? done", result.stdout)
        record = self.records()[0]
        self.assertTrue(record["output_complete"])
        self.assertIsNone(record["read_error"])
        self.assertIn("caf\u00e9 done", Path(record["log"]).read_text(encoding="utf-8"))
        self.assertIn("1 passed", record["summary"])

    def test_a_line_that_fails_to_echo_does_not_stop_the_reading(self):
        module = load_script()
        run = module.Run(io.StringIO())
        seen = []

        def emit(text):
            seen.append(text)
            if text == "a\n":
                raise RuntimeError("cannot echo")

        run.emit = emit
        run.proc = types.SimpleNamespace(stdout=iter(["a\n", "b\n", "1 passed in 0.10s\n"]))
        run.pump()
        self.assertEqual(seen, ["a\n", "b\n", "1 passed in 0.10s\n"])
        self.assertEqual(run.read_error, "RuntimeError: cannot echo")
        self.assertEqual(run.summary, "1 passed in 0.10s")

    def test_a_signal_that_lands_while_one_is_being_passed_on_is_passed_on_once(self):
        module = load_script()
        module.REPEAT_S = 0  # every signal here is a new event
        run = module.Run(io.StringIO())
        run.proc = types.SimpleNamespace(pid=0, poll=lambda: None)
        sent = []

        def kill_group(sig):
            sent.append(sig)
            if len(sent) == 1:  # a second signal arrives while the first is being sent
                run.on_signal(signal.SIGTERM, None)

        run.kill_group = kill_group
        run.on_signal(signal.SIGINT, None)
        self.assertEqual(sent, [signal.SIGINT, signal.SIGKILL])
        self.assertEqual(run.signals, ["SIGINT", "SIGTERM"])

    def test_output_that_could_not_be_read_is_loud_and_never_green(self):
        suite = self.write_suite("pass", PASS_SUITE)
        prelude = ("class Boom:\n    def search(self, line):\n        raise RuntimeError('boom')\n"
                   "m.SUMMARY_RE = Boom()")
        result = self.run_driver(prelude, "-q", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("test-tools: FAIL pytest's output could not be read to its end: RuntimeError: boom",
                      result.stderr)
        record = self.records()[0]
        self.assertEqual(record["pytest_rc"], 0)
        self.assertFalse(record["output_complete"])
        self.assertEqual(record["read_error"], "RuntimeError: boom")
        log = Path(record["log"]).read_text()
        self.assertIn("1 passed", log)
        self.assertIn("output=incomplete", log)

    def test_a_signal_after_pytest_exits_still_leaves_the_end_record(self):
        prelude = ("real = m.failing_ids\n"
                   "def failing_ids(path):\n"
                   "    os.kill(os.getpid(), signal.SIGTERM)\n"
                   "    return real(path)\n"
                   "m.failing_ids = failing_ids")
        result = self.run_driver(prelude)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual(record["rc"], 1)
        self.assertFalse(record["interrupted"])
        self.assertEqual(record["signals"], ["SIGTERM"])
        self.assertEqual(record["failed"], ["test_sample::test_breaks"])

    def test_a_signal_while_pytest_is_being_started_is_passed_on_once_it_has_started(self):
        prelude = ("class Popen(subprocess.Popen):\n"
                   "    def __init__(self, *a, **k):\n"
                   "        super().__init__(*a, **k)\n"
                   "        if '-m' in a[0] and 'pytest' in a[0]:\n"
                   "            os.kill(os.getpid(), signal.SIGINT)\n"
                   "m.subprocess.Popen = Popen")
        slow = self.write_suite("slow", SLOW_SUITE)
        started = time.monotonic()
        result = self.run_driver(prelude, "-q", "-p", "no:cacheprovider", str(slow),
                                 RUN_TOOL_TESTS_MARKER=str(self.root / "marker"),
                                 RUN_TOOL_TESTS_GO=str(self.root / "go"))
        # Not passed on, pytest would run until the 30 s grace kill.
        self.assertLess(time.monotonic() - started, 15)
        self.assertEqual(result.returncode, 130, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual(record["signals"], ["SIGINT"])
        self.assertIsNotNone(record["pytest_rc"])
        self.assertNotEqual(record["pytest_rc"], -signal.SIGKILL)

    def test_a_closed_stdout_at_launch_loses_the_echo_not_the_record(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(self.suite)],
                                stderr=subprocess.PIPE, text=True, cwd=REPO, env=self.env(self.logs),
                                timeout=120, preexec_fn=lambda: os.close(1))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        record = self.records()[0]
        self.assertEqual(record["failed"], ["test_sample::test_breaks"])
        self.assertIn("1 failed, 1 passed", Path(record["log"]).read_text())

    def test_a_reader_that_goes_away_after_the_end_record_does_not_change_the_exit_code(self):
        suite = self.write_suite("pass", PASS_SUITE)
        prelude = ("real = m.append_record\n"
                   "def append_record(index, record):\n"
                   "    ok = real(index, record)\n"
                   "    if record['event'] == 'end':\n"
                   "        r, w = os.pipe()\n"
                   "        os.close(r)\n"
                   "        os.dup2(w, 1)\n"
                   "        os.close(w)\n"
                   "    return ok\n"
                   "m.append_record = append_record")
        result = self.run_driver(prelude, "-q", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertEqual(self.records()[0]["rc"], 0)

    def test_an_inherited_ignored_hangup_stays_ignored(self):
        # As under nohup: SIGHUP is ignored before the script starts.
        proc = self.start_slow(preexec_fn=lambda: signal.signal(signal.SIGHUP, signal.SIG_IGN))
        proc.send_signal(signal.SIGHUP)
        time.sleep(0.5)
        self.assertIsNone(proc.poll())
        self.go.write_text("")
        stdout, stderr = proc.communicate(timeout=90)
        self.assertEqual(proc.returncode, 0, stdout + stderr)
        record = self.records()[0]
        self.assertEqual(record["signals"], [])
        self.assertFalse(record["interrupted"])

    def test_the_exit_code_names_the_first_of_two_different_signals(self):
        proc = self.start_slow(STUBBORN_SUITE)
        proc.send_signal(signal.SIGHUP)
        time.sleep(1.5)
        self.assertIsNone(proc.poll(), "the stubborn test stopped on the first SIGINT")
        proc.send_signal(signal.SIGTERM)
        record = self.assert_interrupted(proc, "SIGHUP", 128 + signal.SIGHUP, ["SIGHUP", "SIGTERM"])
        self.assertEqual(record["pytest_rc"], -signal.SIGKILL)

    def test_the_same_signal_twice_at_once_is_one_interrupt(self):
        # `just` passes on a SIGTERM its process group also got: the script sees it twice.
        proc = self.start_slow(STUBBORN_SUITE, NEVR_TEST_INTERRUPT_GRACE_S="3")
        sent = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        time.sleep(0.2)  # two pending SIGTERMs would merge into one before delivery
        proc.send_signal(signal.SIGTERM)
        record = self.assert_interrupted(proc, "SIGTERM", 128 + signal.SIGTERM, ["SIGTERM", "SIGTERM"])
        # Not escalated: the stubborn test outlived the second signal and was killed at the grace period.
        self.assertGreater(time.monotonic() - sent, 2.5)
        self.assertEqual(record["pytest_rc"], -signal.SIGKILL)

    def test_a_process_out_of_reach_of_the_kill_is_recorded_as_incomplete_output(self):
        stray = self.write_suite("setsid", SETSID_STRAY_SUITE)
        marker = self.root / "marker"
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-s", "-p", "no:cacheprovider", str(stray)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, cwd=REPO,
                                timeout=120,
                                env=self.env(self.logs, NEVR_TEST_DRAIN_S="1", RUN_TOOL_TESTS_MARKER=str(marker)))
        self.addCleanup(kill_group, int(marker.read_text()))
        self.assertEqual(result.returncode, 0, result.stderr)
        record = self.records()[0]
        self.assertTrue(record["stray_kill_sent"])
        self.assertFalse(record["output_complete"])
        self.assertIn("output=incomplete", Path(record["log"]).read_text())

    @unittest.skipUnless(sys.platform.startswith("linux"), "the parent-death signal is Linux's")
    def test_pytest_dies_with_the_script(self):
        proc = self.start_slow()
        pytest_pid = int(self.marker.read_text().split()[0])  # pytest leads its own session
        os.kill(proc.pid, signal.SIGKILL)
        proc.wait(timeout=30)

        def alive() -> bool:
            try:
                state = Path(f"/proc/{pytest_pid}/stat").read_text().rsplit(") ", 1)[1][:1]
            except (FileNotFoundError, ProcessLookupError):
                return False
            return state != "Z"

        deadline = time.monotonic() + 10
        while alive() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(alive(), "pytest outlived the script")

    def test_a_collection_error_is_named_by_its_file(self):
        suite = self.write_suite("broken", "def test_x(:\n    pass\n")
        result = self.run_script(self.logs, "-q", "-p", "no:cacheprovider", str(suite))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        failed = self.records()[0]["failed"]
        self.assertEqual(len(failed), 1)
        self.assertFalse(failed[0].startswith("::"), failed)
        self.assertIn("test_sample", failed[0])

    def test_a_cut_short_line_in_the_index_keeps_the_next_record_whole(self):
        self.logs.mkdir()
        index = self.logs / "runs.jsonl"
        index.write_text('{"event": "end", "cut sho')
        self.run_script(self.logs)
        lines = index.read_text().splitlines()
        self.assertEqual(lines[0], '{"event": "end", "cut sho')
        self.assertEqual([json.loads(l)["event"] for l in lines[1:]], ["start", "end"])

    @unittest.skipIf(importlib.util.find_spec("pytest_randomly") is None, "pytest-randomly is not installed")
    def test_no_seed_when_plugins_do_not_load_automatically(self):
        module = load_script()
        off = {"PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1"}
        self.assertEqual(module.randomly_seed([], off), ([], None))
        extra, seed = module.randomly_seed(["-p", "randomly"], off)
        self.assertIsInstance(seed, int)
        self.assertEqual(extra, [f"--randomly-seed={seed}"])

    def test_unbalanced_pytest_addopts_does_not_crash_the_script(self):
        module = load_script()
        extra, seed = module.randomly_seed([], {"PYTEST_ADDOPTS": "-k 'unclosed"})
        self.assertEqual(len(extra), 0 if seed is None else 1)

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
