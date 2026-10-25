"""tools/run_tool_tests.py: every tools/tests run leaves a record that names its failures (#913).

These tests run the real script, as `just test-tools` does, against small suites written for each case.
"""

import datetime
import importlib.metadata
import importlib.util
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
        self.assertFalse(record["stray_killed"])
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

    def start_slow(self, body: str = SLOW_SUITE, *extra_args: str, **extra_env: str):
        """Start the wrapper in its own session on a suite that waits; return it once the test runs."""
        slow = self.write_suite("slow", body)
        self.marker = self.root / "marker"
        self.go = self.root / "go"
        env = self.env(self.logs, RUN_TOOL_TESTS_MARKER=str(self.marker), RUN_TOOL_TESTS_GO=str(self.go),
                       **extra_env)
        argv = [sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", *extra_args, str(slow)]
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                cwd=REPO, env=env, start_new_session=True)
        self.addCleanup(lambda: proc.poll() is None and os.killpg(proc.pid, signal.SIGKILL))
        deadline = time.monotonic() + 60
        while not self.marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(self.marker.exists(), "the slow test never started")
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
        time.sleep(1)
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
        self.assertLess(time.monotonic() - started, 60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertTrue(record["stray_killed"])
        self.assertTrue(record["output_complete"])
        self.assertIn("1 passed", record["summary"])

    def test_output_written_shortly_after_pytest_exits_is_kept(self):
        # A child still writing for a moment after pytest exits is waited for, not killed.
        late = self.write_suite("late", "import subprocess\n\ndef test_leaves_a_writer():\n"
                                "    subprocess.Popen(['sh', '-c', 'sleep 1; echo LATE-LINE'])\n")
        result = self.run_script(self.logs, "-q", "-s", "-p", "no:cacheprovider", str(late))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertFalse(record["stray_killed"])
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
