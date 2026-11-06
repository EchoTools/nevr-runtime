"""tools/run_tool_tests.py keeps one run at a time, and says what the kernel did to a run (#2195).

The cgroup reads take their root as a parameter, so these tests drive a fake cgroup directory: a tree of `memory.*`
files below a temporary root, at the same relative path as this process's own cgroup (the runner they start finds its
cgroup in `/proc/self/cgroup`, as this process does). The machine-wide lock is a file in the log directory, so each
test's temporary log directory has a lock of its own. A test that waits on the lock's deadline runs the code in a
subprocess with a timeout, so a broken timer fails the test and does not hang it.
"""

import fcntl
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "run_tool_tests.py"

PASS_SUITE = '''
def test_holds():
    assert True
'''

# Waits for $RUN_TOOL_TESTS_GO, so the test can change the fake cgroup while pytest runs.
SLOW_SUITE = '''
import os, pathlib, time

def test_waits():
    marker = pathlib.Path(os.environ["RUN_TOOL_TESTS_MARKER"])
    tmp = marker.with_suffix(".tmp")
    tmp.write_text(str(os.getpid()))
    tmp.rename(marker)
    go = pathlib.Path(os.environ["RUN_TOOL_TESTS_GO"])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and not go.exists():
        time.sleep(0.05)
'''

# Passes only when the runner still holds the lock and pytest did not inherit it.
LOCK_PROBE_SUITE = '''
import fcntl, os

def test_the_lock_is_held_by_the_runner_and_not_by_pytest():
    path = os.environ["RUN_TOOL_TESTS_LOCK"]
    fd = os.open(path, os.O_RDWR)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            raise AssertionError("the runner does not hold the lock while pytest runs")
    finally:
        os.close(fd)
    for name in os.listdir("/proc/self/fd"):
        try:
            target = os.readlink("/proc/self/fd/" + name)
        except OSError:
            continue
        assert not target.endswith("run.lock"), "pytest inherited the lock: " + target
'''

# Takes the lock the way the runner does, in a process of its own: the arguments are the log directory, the deadline in
# seconds and the seconds to stay alive after the lock is taken, counted from the start (a timer left running past the
# deadline would end this process); prints what `acquire` returned.
ACQUIRE_SCRIPT = '''
import importlib.util, json, pathlib, sys, time
spec = importlib.util.spec_from_file_location("run_tool_tests", {script!r})
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
log_dir, deadline, stay = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
start = time.monotonic()
lock = m.MachineLock()
info = lock.acquire(pathlib.Path(log_dir), deadline)
while time.monotonic() < start + stay:
    time.sleep(0.05)
print(json.dumps(info))
lock.release()
'''


def load_script():
    spec = importlib.util.spec_from_file_location("run_tool_tests", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def own_cgroup_path() -> str:
    """The cgroup this process runs in (what a runner started by it runs in as well), or None."""
    for line in Path("/proc/self/cgroup").read_text().splitlines():
        if line.startswith("0::"):
            return line[3:]
    return None


def write(root: Path, cgroup: str, **files) -> Path:
    """Write `files` below root/cgroup; a keyword spells the file name's dot as `__` (`memory__max` is `memory.max`)."""
    directory = root / cgroup.lstrip("/")
    directory.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (directory / name.replace("__", ".")).write_text(str(text) + "\n", encoding="utf-8")
    return directory


def events(oom=0, oom_kill=0) -> str:
    return f"low 0\nhigh 0\nmax 0\noom {oom}\noom_kill {oom_kill}\noom_group_kill 0"


class Tmp(unittest.TestCase):
    def setUp(self):
        # An ignored signal survives exec, and the code under test keeps it ignored (as under nohup); a background
        # job starts with SIGINT ignored. The code under test starts with the defaults.
        for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            if signal.getsignal(s) is signal.SIG_IGN:
                signal.signal(s, signal.SIG_DFL)
                self.addCleanup(signal.signal, s, signal.SIG_IGN)
        self.root = Path(tempfile.mkdtemp(prefix="run-tool-tests-memory-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.module = load_script()


class SubsetTest(Tmp):
    """Which command lines run the whole suite (and so take the lock), classified from the repository root."""

    HEAVY = "tools/tests/test_heavy_guard.py"

    def setUp(self):
        super().setUp()
        self.file = self.root / "test_x.py"
        self.file.write_text("")
        before = os.getcwd()
        os.chdir(REPO)
        self.addCleanup(os.chdir, before)

    def test_a_few_test_paths_are_a_subset(self):
        one = "tools/tests/test_run_tool_tests.py"
        files = sorted(str(p) for p in self.module.SUITE_DIR.glob("test_*.py"))
        self.assertGreater(len(files), self.module.SUBSET_MAX_FILES + 1)
        for args in ([str(self.file)], ["-q", "-p", "no:cacheprovider", str(self.file)], [f"{self.file}::test_a"],
                     [str(self.root)], [one], [f"{one}::RunToolTestsRecord::test_x"], ["-k", "test_a", one],
                     ["-n", "2", one], ["-q", "--deselect", f"{self.HEAVY}::HeavyGuardTest::test_x", one],
                     [str(self.file), str(self.module.SUITE_DIR / "test_run_tool_tests.py")],
                     files[:self.module.SUBSET_MAX_FILES]):
            with self.subTest(args=args[:3]):
                self.assertTrue(self.module.is_subset(args))

    def test_every_command_line_that_runs_the_whole_suite_takes_the_lock(self):
        files = sorted(str(p) for p in self.module.SUITE_DIR.glob("test_*.py"))
        for args in ([], ["-q"], ["-k", "test_a"], ["-q", "-p", "no:cacheprovider"], [str(self.root / "absent.py")],
                     ["tools/tests"], [str(self.module.SUITE_DIR)], ["-q", str(self.module.SUITE_DIR) + "/"],
                     ["tools"], ["."], [str(REPO)], [str(self.file), str(self.module.SUITE_DIR)],
                     ["--deselect", f"{self.HEAVY}::HeavyGuardTest::test_second_run_waits_for_first"],
                     ["-q", "--ignore", self.HEAVY], ["-q", f"--ignore={self.HEAVY}"], ["--rootdir", "."],
                     ["-q", "--basetemp", "/var/tmp"], ["-o", "cache_dir=tools"], ["-p", "tools"],
                     files, files[:self.module.SUBSET_MAX_FILES + 1]):
            with self.subTest(args=args[:3]):
                self.assertFalse(self.module.is_subset(args))


class OwnCgroupTest(Tmp):
    def test_the_cgroup_is_the_version_2_line(self):
        text = "1:net_cls:/\n0::/user.slice/user-1000.slice/run-p1-i2.scope\n"
        self.assertEqual(self.module.own_cgroup(text), "/user.slice/user-1000.slice/run-p1-i2.scope")
        self.assertEqual(self.module.own_cgroup("0::/\n"), "/")

    def test_a_system_with_only_version_1_lines_has_none(self):
        self.assertIsNone(self.module.own_cgroup("1:name=systemd:/a\n2:memory:/b\n"))
        self.assertIsNone(self.module.own_cgroup(""))


class MemoryWatchTest(Tmp):
    """What the run's own cgroup says at the end, against what it said at the start."""

    CG = "/u/s/run.scope"

    def tree(self, oom=0, oom_kill=0, peak=1234, local=None, slice_oom_kill=0):
        write(self.root, self.CG, memory__peak=peak, memory__events=events(oom, oom_kill))
        if local is not None:
            write(self.root, self.CG, memory__events__local=events(*local))
        write(self.root, "/u/s", memory__events=events(slice_oom_kill, slice_oom_kill))
        return self.root / self.CG.lstrip("/")

    def test_the_peak_is_read_through_a_descriptor_reset_at_the_start(self):
        scope = self.tree(peak=987654321)
        watch = self.module.MemoryWatch(self.root, self.CG)
        # The kernel resets the peak of a descriptor that is written to; a plain file shows the write.
        self.assertTrue((scope / "memory.peak").read_text().startswith("0"))
        (scope / "memory.peak").write_text("777\n")
        report = watch.report()
        self.assertEqual((report["state"], report["cgroup"], report["peak_bytes"], report["peak_since"]),
                         ("ok", self.CG, 777, "run start"))

    @unittest.skipIf(os.geteuid() == 0, "root can write a read-only file")
    def test_a_peak_the_kernel_will_not_reset_is_the_lifetime_peak_and_says_so(self):
        scope = self.tree(peak=4242)
        (scope / "memory.peak").chmod(0o444)
        report = self.module.MemoryWatch(self.root, self.CG).report()
        self.assertEqual((report["peak_bytes"], report["peak_since"]), (4242, "cgroup creation"))

    def test_the_kills_and_the_events_of_the_run_are_counted_from_its_start(self):
        scope = self.tree(oom=5, oom_kill=3)
        watch = self.module.MemoryWatch(self.root, self.CG)
        quiet = watch.report()
        self.assertEqual((quiet["oom"], quiet["oom_kill"], quiet["oom_killed"], quiet["oom_killed_in"]),
                         (0, 0, False, None))
        (scope / "memory.events").write_text(events(oom=8, oom_kill=4))
        report = watch.report()
        self.assertEqual((report["oom"], report["oom_kill"], report["oom_killed"], report["oom_killed_in"]),
                         (3, 1, True, self.CG))

    def test_only_kills_of_processes_in_the_cgroup_itself_count(self):
        # memory.events adds the kills of every cgroup below it; memory.events.local is this cgroup's own.
        scope = self.tree(oom_kill=0, local=(0, 0))
        watch = self.module.MemoryWatch(self.root, self.CG)
        (scope / "memory.events").write_text(events(oom=1, oom_kill=1))  # a cgroup below it had a kill
        self.assertFalse(watch.report()["oom_killed"])
        (scope / "memory.events.local").write_text(events(oom=1, oom_kill=1))
        self.assertTrue(watch.report()["oom_killed"])

    def test_a_kill_counted_only_above_the_cgroup_is_not_the_runs(self):
        # The slice's memory.events counts the kill of every scope below it.
        self.tree()
        watch = self.module.MemoryWatch(self.root, self.CG)
        self.tree(slice_oom_kill=7)
        report = watch.report()
        self.assertEqual((report["oom_kill"], report["oom_killed"]), (0, False))

    def test_a_cgroup_that_cannot_be_read_is_a_state_not_a_crash(self):
        report = self.module.MemoryWatch(self.root / "absent", self.CG).report()
        self.assertEqual((report["state"], report["peak_bytes"], report["oom_killed"]), ("unreadable", None, False))
        report = self.module.MemoryWatch(self.root, None).report()
        self.assertEqual((report["state"], report["oom_killed"]), ("no cgroup v2", False))


class MachineLockTest(Tmp):
    """The lock itself. A wait with a deadline runs in a process of its own, with a timeout."""

    def acquire(self, deadline: float, stay: float = 0.0, log_dir: Path = None) -> dict:
        done = subprocess.run([sys.executable, "-c", ACQUIRE_SCRIPT.format(script=str(SCRIPT)),
                               str(log_dir or self.root), str(deadline), str(stay)],
                              capture_output=True, text=True, timeout=60)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return json.loads(done.stdout)

    def test_the_signal_handlers_are_put_back_after_the_lock_is_taken(self):
        before = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGALRM)}
        lock = self.module.MachineLock()
        lock.acquire(self.root, 5)
        self.addCleanup(lock.release)
        self.assertEqual({s: signal.getsignal(s) for s in before}, before)

    def test_a_released_lock_is_free_for_the_next_run(self):
        first = self.module.MachineLock()
        self.assertEqual(first.acquire(self.root, 5)["state"], "acquired")
        held = self.acquire(0.2)  # held: the second run gives up at its deadline
        self.assertEqual(held["state"], "timeout")
        first.release()
        self.assertEqual(self.acquire(5)["state"], "acquired")

    def test_the_lock_file_names_the_holder_and_the_one_that_gave_up_reports_it(self):
        first = self.module.MachineLock()
        first.acquire(self.root, 5)
        holder = json.loads((self.root / "run.lock").read_text())
        self.assertEqual((holder["pid"], holder["checkout"]), (os.getpid(), self.module.git("rev-parse", "--show-toplevel")))
        self.assertIn("start", holder)
        waited = self.acquire(0.2)
        self.assertEqual((waited["state"], waited["contended"], waited["holder"], waited["taken"]),
                         ("timeout", True, holder, False))
        first.release()
        self.assertEqual((self.root / "run.lock").read_text(), "")

    def test_the_holder_a_refused_run_reports_is_the_one_at_the_deadline(self):
        # The lock is held throughout; the file names another run by the time the wait ends.
        fd = os.open(self.root / "run.lock", os.O_RDWR | os.O_CREAT, 0o644)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.write(fd, json.dumps({"pid": 4242}).encode())
        proc = subprocess.Popen([sys.executable, "-c", ACQUIRE_SCRIPT.format(script=str(SCRIPT)), str(self.root),
                                 "1.0", "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        waiting = proc.stderr.readline()
        self.assertIn('"pid": 4242', waiting)
        os.ftruncate(fd, 0)
        os.pwrite(fd, json.dumps({"pid": 5555}).encode(), 0)
        stdout, stderr = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 0, stdout + stderr)
        info = json.loads(stdout)
        self.assertEqual((info["state"], info["holder"]), ("timeout", {"pid": 5555}))

    def test_a_holder_written_over_a_longer_one_leaves_none_of_it(self):
        (self.root / "run.lock").write_text(json.dumps({"pid": 1, "checkout": "x" * 400, "start": "long ago"}))
        lock = self.module.MachineLock()
        lock.acquire(self.root, 5)
        self.addCleanup(lock.release)
        self.assertEqual(json.loads((self.root / "run.lock").read_text())["pid"], os.getpid())

    def test_the_wait_timer_is_gone_once_the_lock_is_taken(self):
        # A timer left running ends the process (SIGALRM's default action) when the deadline passes: the process here
        # stays alive past the deadline after a wait that ended at the release.
        first = self.module.MachineLock()
        first.acquire(self.root, 5)
        proc = subprocess.Popen([sys.executable, "-c", ACQUIRE_SCRIPT.format(script=str(SCRIPT)), str(self.root),
                                 "1.0", "1.6"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        self.assertIn("waiting for the machine-wide test lock", proc.stderr.readline())
        first.release()
        stdout, stderr = proc.communicate(timeout=60)
        self.assertEqual(proc.returncode, 0, stdout + stderr)
        self.assertEqual(json.loads(stdout)["state"], "acquired")

    def test_a_signal_while_waiting_stops_the_wait(self):
        first = self.module.MachineLock()
        first.acquire(self.root, 5)
        for name in ("SIGTERM", "SIGINT", "SIGHUP"):
            with self.subTest(signal=name):
                proc = subprocess.Popen([sys.executable, "-c", ACQUIRE_SCRIPT.format(script=str(SCRIPT)),
                                         str(self.root), "60", "0"], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
                self.addCleanup(lambda p=proc: p.poll() is None and p.kill())
                self.assertIn("waiting for the machine-wide test lock", proc.stderr.readline())
                proc.send_signal(getattr(signal, name))
                stdout, stderr = proc.communicate(timeout=60)
                self.assertEqual(proc.returncode, 0, stdout + stderr)
                self.assertNotIn("Traceback", stderr)
                info = json.loads(stdout)
                self.assertEqual((info["state"], info["signal"], info["taken"]), ("interrupted", name, False))
        first.release()


@unittest.skipIf(own_cgroup_path() is None, "no cgroup v2 line in /proc/self/cgroup")
class RunnerTest(unittest.TestCase):
    """The real script, over a fake cgroup tree at this process's own cgroup path."""

    def setUp(self):
        for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            if signal.getsignal(s) is signal.SIG_IGN:
                signal.signal(s, signal.SIG_DFL)
                self.addCleanup(signal.signal, s, signal.SIG_IGN)
        self.root = Path(tempfile.mkdtemp(prefix="run-tool-tests-memory-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.cgroups = self.root / "cgroups"
        self.logs = self.root / "logs"
        self.own = own_cgroup_path()
        self.module = load_script()
        self.suite = self.write_suite("suite", PASS_SUITE)

    def write_suite(self, name: str, body: str) -> Path:
        d = self.root / name
        d.mkdir(parents=True)
        (d / "test_sample.py").write_text(body, encoding="utf-8")
        (d / "pytest.ini").write_text("[pytest]\n", encoding="utf-8")
        return d

    def fake(self) -> Path:
        """The run's cgroup (`<own>`), with a parent that has events of its own (a slice)."""
        scope = write(self.cgroups, self.own, memory__peak=4096, memory__events=events())
        write(self.cgroups, str(Path(self.own).parent), memory__events=events())
        return scope

    def env(self, **extra) -> dict:
        env = dict(os.environ, NEVR_TEST_LOG_DIR=str(self.logs), NEVR_TEST_WORKERS="0",
                   NEVR_TEST_CGROUP_ROOT=str(self.cgroups))
        for name in ("PYTEST_ADDOPTS", "PYTEST_DISABLE_PLUGIN_AUTOLOAD", "PYTEST_XDIST_AUTO_NUM_WORKERS",
                     "PYTEST_XDIST_WORKER", "PYTEST_XDIST_WORKER_COUNT", "PYTEST_XDIST_TESTRUNUID"):
            env.pop(name, None)
        env.update({k: v for k, v in extra.items() if v is not None})
        for name in [k for k, v in extra.items() if v is None]:
            env.pop(name, None)
        return env

    def run_script(self, *args: str, **extra) -> subprocess.CompletedProcess:
        argv = list(args) or ["-q", "-p", "no:cacheprovider", str(self.suite)]
        return subprocess.run([sys.executable, str(SCRIPT), *argv], capture_output=True, text=True, cwd=REPO,
                              env=self.env(**extra), timeout=120)

    def run_whole(self, suite: Path = None, **extra) -> subprocess.CompletedProcess:
        """A run of the whole suite as the runner sees it (no argument names a test path); pytest is pointed at the
        test suite by $PYTEST_ADDOPTS, which the runner does not read for that."""
        return subprocess.run([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider"], capture_output=True,
                              text=True, cwd=REPO, timeout=120,
                              env=self.env(PYTEST_ADDOPTS=str(suite or self.suite), **extra))

    def popen_whole(self, suite: Path = None, **extra) -> subprocess.Popen:
        return self.popen("-q", "-p", "no:cacheprovider", PYTEST_ADDOPTS=str(suite or self.suite), **extra)

    def popen(self, *args: str, **extra) -> subprocess.Popen:
        argv = list(args) or ["-q", "-p", "no:cacheprovider", str(self.suite)]
        proc = subprocess.Popen([sys.executable, str(SCRIPT), *argv], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, cwd=REPO, env=self.env(**extra), start_new_session=True)
        self.addCleanup(lambda: proc.poll() is None and os.killpg(proc.pid, signal.SIGKILL))
        return proc

    def records(self, event: str = "end") -> list:
        lines = (self.logs / "runs.jsonl").read_text().splitlines()
        return [r for r in map(json.loads, lines) if r["event"] == event]

    def start_slow(self, whole: bool = False, **extra):
        """A runner on a suite that waits; returned once pytest runs, so the runner has taken its lock (a run of the
        whole suite, `whole`) and started its watch."""
        slow = self.write_suite("slow", SLOW_SUITE)
        self.marker = self.root / "marker"
        self.go = self.root / "go"
        env = dict(RUN_TOOL_TESTS_MARKER=str(self.marker), RUN_TOOL_TESTS_GO=str(self.go), **extra)
        proc = (self.popen_whole(slow, **env) if whole else
                self.popen("-q", "-p", "no:cacheprovider", str(slow), **env))
        deadline = time.monotonic() + 60
        while not self.marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(self.marker.exists(), "the slow test never started")
        self.addCleanup(lambda: self.go.write_text(""))
        return proc

    # The memory the end record keeps.

    def test_the_end_record_and_the_log_carry_the_runs_peak_and_events(self):
        scope = self.fake()
        proc = self.start_slow()
        (scope / "memory.peak").write_text("123456789\n")  # the runner reset it when it started
        self.go.write_text("")
        stdout, stderr = proc.communicate(timeout=120)
        self.assertEqual(proc.returncode, 0, stdout + stderr)
        record = self.records()[0]
        memory = record["memory"]
        self.assertEqual((memory["state"], memory["cgroup"], memory["peak_bytes"], memory["peak_since"]),
                         ("ok", self.own, 123456789, "run start"))
        self.assertEqual((memory["oom"], memory["oom_kill"], memory["oom_killed"]), (0, 0, False))
        self.assertFalse(record["oom_killed"])
        log = Path(record["log"]).read_text()
        self.assertIn("test-tools: memory peak=123456789 since='run start' ", log)
        self.assertRegex(log, r"\ntest-tools: end \S+ rc=0\n$")
        self.assertNotIn("limit", json.dumps(record["memory"]))

    def test_a_run_with_no_readable_cgroup_says_so_and_still_runs(self):
        result = self.run_script()  # no fake tree: the root does not exist
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        memory = self.records()[0]["memory"]
        self.assertEqual((memory["state"], memory["peak_bytes"]), ("unreadable", None))

    def test_an_interrupted_run_whose_cgroup_saw_an_oom_kill_is_recorded_as_oom_killed(self):
        scope = self.fake()
        proc = self.start_slow()
        # The kernel kills a process in the scope while pytest runs; systemd then stops the rest with SIGTERM.
        (scope / "memory.events").write_text(events(oom=1, oom_kill=1))
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=90)
        self.assertEqual(proc.returncode, 128 + signal.SIGTERM, stdout + stderr)
        record = self.records()[0]
        self.assertTrue(record["interrupted"])
        self.assertEqual(record["signals"], ["SIGTERM"])
        self.assertEqual((record["oom_killed"], record["oom_killed_in"], record["memory"]["oom_kill"],
                          record["memory"]["oom"]), (True, self.own, 1, 1))
        self.assertIn(f"interrupted=SIGTERM oom_killed={self.own}", Path(record["log"]).read_text())
        self.assertIn("test-tools: FAIL the kernel's OOM killer stopped this run", stderr)

    def test_an_interrupted_run_with_no_oom_kill_stays_a_plain_signal(self):
        self.fake()
        proc = self.start_slow()
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=90)
        record = self.records()[0]
        self.assertEqual((record["interrupted"], record["oom_killed"], record["oom_killed_in"]), (True, False, None))
        self.assertNotIn("oom_killed", Path(record["log"]).read_text())
        self.assertNotIn("OOM", stderr)

    def test_a_kill_in_another_scope_of_the_slice_is_not_this_runs(self):
        # The slice counts the kill of every scope below it; this run's scope counted none, so a SIGTERM it gets
        # (a tool's timeout, CTRL+C) is a plain interrupt.
        self.fake()
        slice_events = self.cgroups / str(Path(self.own).parent).lstrip("/") / "memory.events"
        proc = self.start_slow()
        slice_events.write_text(events(oom=1, oom_kill=1))
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=90)
        record = self.records()[0]
        self.assertEqual((record["interrupted"], record["oom_killed"]), (True, False))
        self.assertEqual(record["memory"]["oom_kill"], 0)
        self.assertNotIn("OOM", stderr)

    def test_pytest_killed_by_the_kernel_in_the_runs_cgroup_is_recorded_as_oom_killed(self):
        # The kernel's victim is pytest itself: SIGKILL, with no signal sent to the runner.
        scope = self.fake()
        proc = self.start_slow()
        (scope / "memory.events").write_text(events(oom=1, oom_kill=1))
        os.kill(int(self.marker.read_text()), signal.SIGKILL)
        stdout, stderr = proc.communicate(timeout=90)
        self.assertEqual(proc.returncode, 128 + signal.SIGKILL, stdout + stderr)
        record = self.records()[0]
        self.assertEqual((record["interrupted"], record["pytest_rc"]), (False, -signal.SIGKILL))
        self.assertEqual((record["oom_killed"], record["oom_killed_in"]), (True, self.own))
        self.assertIn(f"oom_killed={self.own}", Path(record["log"]).read_text())

    # The worker count.

    @unittest.skipIf(importlib.util.find_spec("xdist") is None, "pytest-xdist is not installed")
    def test_the_default_worker_count_is_one_per_thread_and_at_most_the_ceiling(self):
        ceiling = self.module.DEFAULT_WORKER_CEILING
        cases = ((str(ceiling + 4), ceiling, "ceiling"), (str(ceiling), ceiling, "cpu"), ("3", 3, "cpu"))
        for i, (threads, workers, limited_by) in enumerate(cases):
            with self.subTest(threads=threads):
                self.logs = self.root / f"logs{i}"
                result = self.run_script(NEVR_TEST_WORKERS=None, PYTEST_XDIST_AUTO_NUM_WORKERS=threads)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for record in self.records("start") + self.records():
                    self.assertEqual((record["mode"], record["workers"], record["workers_source"]),
                                     ("parallel", workers, "default"))
                    self.assertEqual(record["workers_basis"], {"cpus": int(threads), "ceiling": ceiling,
                                                               "workers": workers, "limited_by": limited_by})
                    self.assertEqual(record["cmd"][record["cmd"].index("-n") + 1], str(workers))
                self.assertIn("test-tools: worker basis source=default ", Path(self.records()[0]["log"]).read_text())

    @unittest.skipIf(importlib.util.find_spec("xdist") is None, "pytest-xdist is not installed")
    def test_an_explicit_worker_count_wins_and_logical_means_every_thread(self):
        cases = (  # (arguments, environment, workers, source)
            ([], {"NEVR_TEST_WORKERS": "3"}, 3, "NEVR_TEST_WORKERS"),
            (["-n", "2"], {"NEVR_TEST_WORKERS": "3"}, 2, "-n in the arguments"),
            (["-n", "2"], {"NEVR_TEST_WORKERS": None}, 2, "-n in the arguments"),
            ([], {"NEVR_TEST_WORKERS": "logical", "PYTEST_XDIST_AUTO_NUM_WORKERS": "11"}, 11, "NEVR_TEST_WORKERS"),
        )
        for i, (given, env, workers, source) in enumerate(cases):
            with self.subTest(arguments=given, environment=env):
                self.logs = self.root / f"logs{i}"
                result = self.run_script("-q", "-p", "no:cacheprovider", *given, str(self.suite), **env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for record in self.records("start") + self.records():
                    self.assertEqual((record["workers"], record["workers_source"], record["workers_basis"]),
                                     (workers, source, None))

    def test_a_workers_setting_of_the_word_default_is_refused(self):
        result = self.run_script(NEVR_TEST_WORKERS="default")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("is not auto, logical or a whole number", result.stderr)

    # The machine-wide lock.

    def hold_lock(self, holder: dict = None) -> int:
        self.logs.mkdir(parents=True, exist_ok=True)
        fd = os.open(self.logs / "run.lock", os.O_RDWR | os.O_CREAT, 0o644)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if holder:
            os.write(fd, json.dumps(holder).encode())
        return fd

    def test_an_uncontended_run_records_that_it_held_the_lock_and_waited_no_time(self):
        result = self.run_whole()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for record in self.records("start") + self.records():
            lock = record["lock"]
            self.assertEqual((lock["taken"], lock["state"], lock["path"], lock["contended"], lock["holder"]),
                             (True, "acquired", str(self.logs / "run.lock"), False, None))
            self.assertLess(lock["wait_s"], 5)
            self.assertEqual(lock["deadline_s"], 5400)

    def test_a_run_given_test_paths_takes_no_lock_and_does_not_wait_for_one(self):
        self.hold_lock({"pid": 4242, "checkout": "/the/other/checkout"})
        before = (self.logs / "run.lock").read_text()
        result = self.run_script(NEVR_TEST_LOCK_WAIT_S="0.5")  # run_script names the suite: a subset
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("waiting for the machine-wide test lock", result.stderr)
        for record in self.records("start") + self.records():
            self.assertEqual(record["lock"], {"taken": False, "reason": "subset"})
        self.assertIn("test-tools: lock not taken (subset", Path(self.records()[0]["log"]).read_text())
        self.assertEqual((self.logs / "run.lock").read_text(), before)

    def test_a_run_of_the_whole_suite_waits_for_the_lock_a_subset_leaves_alone(self):
        self.hold_lock({"pid": 4242, "checkout": "/the/other/checkout"})
        result = self.run_whole(NEVR_TEST_LOCK_WAIT_S="0.5")
        self.assertEqual(result.returncode, 75, result.stdout + result.stderr)
        self.assertEqual(self.records("lock_timeout")[0]["lock"]["taken"], False)

    def test_a_run_that_cannot_get_the_lock_by_its_deadline_is_refused_and_names_who_holds_it(self):
        holder = self.popen_holder()
        result = self.run_whole(NEVR_TEST_LOCK_WAIT_S="0.5")
        self.assertEqual(result.returncode, 75, result.stdout + result.stderr)
        self.assertIn("test-tools: FAIL the machine-wide test lock", result.stderr)
        self.assertIn(f'"pid": {holder.pid}', result.stderr)
        self.assertIn(str(REPO), result.stderr)
        self.assertNotIn("passed", result.stdout)
        self.assertNotIn("Traceback", result.stderr)
        self.assertEqual(self.records("start")[0]["event"], "start")  # the holder's own
        refused = self.records("lock_timeout")
        self.assertEqual(len(refused), 1)
        self.assertEqual((refused[0]["lock"]["state"], refused[0]["lock"]["deadline_s"], refused[0]["lock"]["taken"]),
                         ("timeout", 0.5, False))
        self.assertGreaterEqual(refused[0]["lock"]["wait_s"], 0.5)
        self.assertEqual((refused[0]["lock"]["holder"]["pid"], refused[0]["lock"]["holder"]["checkout"]),
                         (holder.pid, str(REPO)))

    def popen_holder(self) -> subprocess.Popen:
        """A real run that holds the lock until the test ends (its suite waits for $RUN_TOOL_TESTS_GO)."""
        proc = self.start_slow(whole=True)
        return proc

    def test_the_lock_file_of_a_running_run_names_that_run(self):
        proc = self.popen_holder()
        holder = json.loads((self.logs / "run.lock").read_text())
        self.assertEqual((holder["pid"], holder["checkout"]), (proc.pid, str(REPO)))
        self.go.write_text("")
        proc.communicate(timeout=120)
        self.assertEqual((self.logs / "run.lock").read_text(), "")

    def test_a_run_waits_for_the_lock_and_records_how_long_and_behind_whom(self):
        fd = self.hold_lock({"pid": 4242, "checkout": "/the/other/checkout"})
        proc = self.popen_whole(NEVR_TEST_LOCK_WAIT_S="60")
        waiting = proc.stderr.readline()  # written before it blocks, so the lock is released only after it
        self.assertIn("test-tools: waiting for the machine-wide test lock", waiting)
        self.assertIn("4242", waiting)
        fcntl.flock(fd, fcntl.LOCK_UN)
        stdout, stderr = proc.communicate(timeout=120)
        self.assertEqual(proc.returncode, 0, stdout + stderr)
        lock = self.records("start")[0]["lock"]
        self.assertEqual((lock["state"], lock["deadline_s"], lock["contended"], lock["taken"]),
                         ("acquired", 60, True, True))
        self.assertEqual(lock["holder"], {"pid": 4242, "checkout": "/the/other/checkout"})
        self.assertGreater(lock["wait_s"], 0)

    def test_a_signal_while_the_run_waits_for_the_lock_leaves_a_record_and_no_traceback(self):
        for name in ("SIGTERM", "SIGINT", "SIGHUP"):
            with self.subTest(signal=name):
                self.logs = self.root / f"logs-{name}"
                self.hold_lock({"pid": 4242, "checkout": "/the/other/checkout"})
                proc = self.popen_whole(NEVR_TEST_LOCK_WAIT_S="60")
                self.assertIn("waiting for the machine-wide test lock", proc.stderr.readline())
                proc.send_signal(getattr(signal, name))
                stdout, stderr = proc.communicate(timeout=60)
                self.assertEqual(proc.returncode, 128 + getattr(signal, name).value, stdout + stderr)
                self.assertNotIn("Traceback", stderr)
                self.assertIn(f"{name} stopped this run", stderr)
                self.assertNotIn("passed", stdout)
                self.assertEqual(self.records("start"), [])
                stopped = self.records("lock_wait_interrupted")
                self.assertEqual(len(stopped), 1)
                lock = stopped[0]["lock"]
                self.assertEqual((lock["state"], lock["signal"], lock["holder"]["pid"], lock["taken"]),
                                 ("interrupted", name, 4242, False))
                self.assertGreater(lock["wait_s"], 0)

    def test_the_lock_is_held_during_the_run_and_pytest_does_not_inherit_it(self):
        probe = self.write_suite("probe", LOCK_PROBE_SUITE)
        result = self.run_whole(probe, RUN_TOOL_TESTS_LOCK=str(self.logs / "run.lock"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_the_lock_is_released_when_the_run_ends(self):
        self.assertEqual(self.run_whole().returncode, 0)
        fd = os.open(self.logs / "run.lock", os.O_RDWR)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)  # raises when the finished run still holds it
        self.assertEqual((self.logs / "run.lock").read_text(), "")  # and the file names no holder

    def test_each_log_directory_has_a_lock_of_its_own(self):
        # A run a test starts keeps its records in a directory of its own, so it never waits for the run that
        # started the test.
        self.hold_lock()
        other = self.root / "other-logs"
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider"],
                                capture_output=True, text=True, cwd=REPO, timeout=120,
                                env=self.env(NEVR_TEST_LOG_DIR=str(other), NEVR_TEST_LOCK_WAIT_S="0.5",
                                             PYTEST_ADDOPTS=str(self.suite)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        start = [json.loads(line) for line in (other / "runs.jsonl").read_text().splitlines()][0]
        self.assertEqual((start["lock"]["path"], start["lock"]["contended"]), (str(other / "run.lock"), False))

    def test_a_lock_wait_that_is_not_a_positive_number_of_seconds_refuses_the_run(self):
        for value in ("0", "inf", "soon"):
            with self.subTest(value=value):
                self.logs = self.root / f"logs-{value}"
                result = self.run_script(NEVR_TEST_LOCK_WAIT_S=value)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn(f"test-tools: FAIL $NEVR_TEST_LOCK_WAIT_S={value!r} is not a positive number of seconds",
                              result.stderr)


if __name__ == "__main__":
    unittest.main()
