"""tools/run_tool_tests.py keeps a run inside the memory of the cgroup it runs in, and says what the kernel did (#2195).

The cgroup reads take their root as a parameter, so these tests drive a fake cgroup directory: a tree of `memory.*`
files below a temporary root, at the same relative path as this process's own cgroup (the runner they start finds its
cgroup in `/proc/self/cgroup`, as this process does). The machine-wide lock is a file in the log directory, so each
test's temporary log directory has a lock of its own.
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

GIB = 1024 ** 3

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


class Cgroup(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="run-tool-tests-memory-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.module = load_script()


class OwnCgroupTest(Cgroup):
    def test_the_cgroup_is_the_version_2_line(self):
        text = "1:net_cls:/\n0::/user.slice/user-1000.slice/run-p1-i2.scope\n"
        self.assertEqual(self.module.own_cgroup(text), "/user.slice/user-1000.slice/run-p1-i2.scope")
        self.assertEqual(self.module.own_cgroup("0::/\n"), "/")

    def test_a_system_with_only_version_1_lines_has_none(self):
        self.assertIsNone(self.module.own_cgroup("1:name=systemd:/a\n2:memory:/b\n"))
        self.assertIsNone(self.module.own_cgroup(""))


class MemoryLimitTest(Cgroup):
    def test_the_tightest_headroom_up_the_chain_is_the_limit(self):
        # The scope has 8 GiB and uses 1; the slice above it has 6 and its other users hold 4 of them.
        write(self.root, "/u/slice/run.scope", memory__max=8 * GIB, memory__current=1 * GIB)
        write(self.root, "/u/slice", memory__max=6 * GIB, memory__current=4 * GIB)
        write(self.root, "/u", memory__max="max", memory__current=9 * GIB)
        limit = self.module.memory_limit(self.root, "/u/slice/run.scope")
        self.assertEqual((limit["cgroup"], limit["max_bytes"], limit["used_bytes"], limit["headroom_bytes"]),
                         ("/u/slice", 6 * GIB, 4 * GIB, 2 * GIB))

    def test_the_scope_is_the_limit_when_it_is_the_tightest(self):
        write(self.root, "/u/slice/run.scope", memory__max=3 * GIB, memory__current=1 * GIB)
        write(self.root, "/u/slice", memory__max=6 * GIB, memory__current=1 * GIB)
        limit = self.module.memory_limit(self.root, "/u/slice/run.scope")
        self.assertEqual((limit["cgroup"], limit["headroom_bytes"]), ("/u/slice/run.scope", 2 * GIB))

    def test_page_cache_the_kernel_gives_back_is_not_counted_as_used(self):
        write(self.root, "/s", memory__max=6 * GIB, memory__current=5 * GIB,
              memory__stat="anon 100\ninactive_file %d\nactive_file 7\n" % (3 * GIB))
        limit = self.module.memory_limit(self.root, "/s")
        self.assertEqual((limit["used_bytes"], limit["headroom_bytes"]), (2 * GIB, 4 * GIB))

    def test_a_chain_with_no_limit_has_none(self):
        write(self.root, "/a/b", memory__max="max", memory__current=1)
        write(self.root, "/a", memory__max="max", memory__current=1)
        self.assertIsNone(self.module.memory_limit(self.root, "/a/b"))

    def test_a_missing_or_unreadable_tree_has_none(self):
        self.assertIsNone(self.module.memory_limit(self.root / "absent", "/a/b"))
        write(self.root, "/a", memory__max="not a number", memory__current="x")
        self.assertIsNone(self.module.memory_limit(self.root, "/a"))
        self.assertIsNone(self.module.memory_limit(self.root, None))


class FitWorkersTest(Cgroup):
    def limit(self, headroom: int) -> dict:
        return {"cgroup": "/s", "max_bytes": 6 * GIB, "used_bytes": 6 * GIB - headroom, "headroom_bytes": headroom}

    def test_the_count_is_the_smaller_of_the_cpus_and_what_the_memory_holds(self):
        m = self.module
        base, per = m.MEMORY_BASE_BYTES, m.MEMORY_PER_WORKER_BYTES
        cases = (  # (cpus, headroom, workers, limited_by)
            (32, base + 5 * per + per // 2, 5, "memory"),
            (4, base + 5 * per + per // 2, 4, "cpu"),
            (4, base + 4 * per, 4, "cpu"),  # exactly enough for the cpus: the cpus bound it, not the memory
            (32, base + per, 1, "memory"),
        )
        for cpus, headroom, workers, limited_by in cases:
            with self.subTest(cpus=cpus, headroom=headroom):
                fit = m.fit_workers(cpus, self.limit(headroom))
                self.assertEqual((fit["workers"], fit["limited_by"]), (workers, limited_by))
                self.assertEqual((fit["cpus"], fit["base_bytes"], fit["per_worker_bytes"]),
                                 (cpus, base, per))

    def test_less_than_one_worker_of_headroom_still_runs_one(self):
        for headroom in (self.module.MEMORY_BASE_BYTES // 2, 0, -1):
            with self.subTest(headroom=headroom):
                fit = self.module.fit_workers(32, self.limit(headroom))
                self.assertEqual((fit["workers"], fit["limited_by"]), (1, "memory"))

    def test_with_no_memory_limit_the_cpus_decide_and_the_record_says_so(self):
        fit = self.module.fit_workers(12, None)
        self.assertEqual((fit["workers"], fit["limited_by"], fit["limit_cgroup"]), (12, "cpu", None))
        self.assertIn("no memory limit", fit["memory"])

    def test_the_basis_names_the_cgroup_and_the_numbers_it_used(self):
        fit = self.module.fit_workers(32, self.limit(2 * GIB))
        self.assertEqual((fit["limit_cgroup"], fit["headroom_bytes"], fit["memory_workers"]),
                         ("/s", 2 * GIB, max(1, (2 * GIB - self.module.MEMORY_BASE_BYTES)
                                             // self.module.MEMORY_PER_WORKER_BYTES)))


class MemoryWatchTest(Cgroup):
    """What the run's cgroup says at its end, against what it said at its start."""

    def tree(self, own_oom_kill=0, limit_oom_kill=0, peak=1234):
        write(self.root, "/u/s/run.scope", memory__max=8 * GIB, memory__current=100, memory__peak=peak,
              memory__events=f"low 0\nhigh 0\nmax 0\noom 0\noom_kill {own_oom_kill}\n")
        write(self.root, "/u/s", memory__max=6 * GIB, memory__current=200, memory__peak=5678,
              memory__events=f"low 0\nhigh 0\nmax 0\noom 0\noom_kill {limit_oom_kill}\n")

    def watch(self):
        self.tree()
        return self.module.MemoryWatch(self.root, "/u/s/run.scope")

    def test_the_peak_and_the_events_of_the_scope_are_reported(self):
        watch = self.watch()
        report = watch.report(interrupted=False)
        self.assertEqual((report["state"], report["cgroup"], report["peak_bytes"]), ("ok", "/u/s/run.scope", 1234))
        self.assertEqual((report["limit_cgroup"], report["limit_peak_bytes"]), ("/u/s", 5678))
        self.assertEqual((report["oom_kill"], report["limit_oom_kill"], report["oom_killed"],
                          report["oom_killed_in"]), (0, 0, False, None))

    def test_an_oom_kill_in_the_scope_during_the_run_is_an_oom_kill_of_the_run(self):
        watch = self.watch()
        self.tree(own_oom_kill=2)
        report = watch.report(interrupted=False)
        self.assertEqual((report["oom_kill"], report["oom_killed"], report["oom_killed_in"]),
                         (2, True, "/u/s/run.scope"))

    def test_events_before_the_run_are_not_its_own(self):
        self.tree(own_oom_kill=3, limit_oom_kill=9)
        watch = self.module.MemoryWatch(self.root, "/u/s/run.scope")
        report = watch.report(interrupted=True)
        self.assertEqual((report["oom_kill"], report["limit_oom_kill"], report["oom_killed"]), (0, 0, False))

    def test_a_kill_in_the_limiting_cgroup_explains_an_interrupted_run_only(self):
        watch = self.watch()
        self.tree(limit_oom_kill=1)
        quiet = watch.report(interrupted=False)
        self.assertEqual((quiet["limit_oom_kill"], quiet["oom_killed"]), (1, False))
        loud = watch.report(interrupted=True)
        self.assertEqual((loud["oom_killed"], loud["oom_killed_in"]), (True, "/u/s"))

    def test_a_cgroup_that_cannot_be_read_is_a_state_not_a_crash(self):
        report = self.module.MemoryWatch(self.root / "absent", "/u/s/run.scope").report(interrupted=True)
        self.assertEqual((report["state"], report["peak_bytes"], report["oom_killed"]), ("unreadable", None, False))
        report = self.module.MemoryWatch(self.root, None).report(interrupted=True)
        self.assertEqual((report["state"], report["oom_killed"]), ("no cgroup v2", False))


class MachineLockTest(Cgroup):
    def test_a_released_lock_is_free_for_the_next_run_in_the_same_process(self):
        first, second, third = (self.module.MachineLock() for _ in range(3))
        self.assertEqual(first.acquire(self.root, 5)["state"], "acquired")
        # Held: a second run gives up at its deadline (a lock held by a file descriptor still open is held).
        self.assertEqual(second.acquire(self.root, 0.2)["state"], "timeout")
        first.release()
        self.assertEqual(third.acquire(self.root, 5)["state"], "acquired")
        third.release()


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

    def fake(self, headroom=None, peak=None, oom_kill=0, limit_oom_kill=0) -> Path:
        """A tree where the run's cgroup is `<own>` and its parent is the limit: `headroom` bytes free."""
        parent = str(Path(self.own).parent)
        write(self.cgroups, self.own, memory__max="max", memory__current=1000,
              memory__peak=peak if peak is not None else 4096,
              memory__events=f"low 0\nhigh 0\nmax 0\noom 0\noom_kill {oom_kill}\n")
        if headroom is not None:
            write(self.cgroups, parent, memory__max=6 * GIB, memory__current=6 * GIB - headroom,
                  memory__peak=6 * GIB - headroom,
                  memory__events=f"low 0\nhigh 0\nmax 0\noom 0\noom_kill {limit_oom_kill}\n")
        return self.cgroups / self.own.lstrip("/")

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

    def records(self, event: str = "end") -> list:
        lines = (self.logs / "runs.jsonl").read_text().splitlines()
        return [r for r in map(json.loads, lines) if r["event"] == event]

    # The memory the end record keeps.

    def test_the_end_record_and_the_log_carry_the_scopes_peak_and_events(self):
        self.fake(headroom=3 * GIB, peak=123456789)
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        memory = self.records()[0]["memory"]
        self.assertEqual((memory["state"], memory["cgroup"], memory["peak_bytes"]), ("ok", self.own, 123456789))
        self.assertEqual((memory["oom_kill"], memory["oom_killed"]), (0, False))
        self.assertFalse(self.records()[0]["oom_killed"])
        log = Path(self.records()[0]["log"]).read_text()
        self.assertIn("test-tools: memory peak=123456789 ", log)
        self.assertRegex(log, r"\ntest-tools: end \S+ rc=0\n$")

    def test_a_run_with_no_readable_cgroup_says_so_and_still_runs(self):
        result = self.run_script()  # no fake tree: the root does not exist
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        memory = self.records()[0]["memory"]
        self.assertEqual((memory["state"], memory["peak_bytes"]), ("unreadable", None))

    def start_slow(self, **extra):
        slow = self.write_suite("slow", SLOW_SUITE)
        self.marker = self.root / "marker"
        self.go = self.root / "go"
        env = self.env(RUN_TOOL_TESTS_MARKER=str(self.marker), RUN_TOOL_TESTS_GO=str(self.go), **extra)
        proc = subprocess.Popen([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(slow)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=REPO, env=env,
                                start_new_session=True)
        self.addCleanup(lambda: proc.poll() is None and os.killpg(proc.pid, signal.SIGKILL))
        deadline = time.monotonic() + 60
        while not self.marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(self.marker.exists(), "the slow test never started")
        self.addCleanup(lambda: self.go.write_text(""))
        return proc

    def test_an_interrupted_run_whose_cgroup_saw_an_oom_kill_is_recorded_as_oom_killed(self):
        scope = self.fake(headroom=3 * GIB)
        proc = self.start_slow()
        # The kernel kills a process in the scope while pytest runs; systemd then stops the rest with SIGTERM.
        (scope / "memory.events").write_text("low 0\nhigh 0\nmax 0\noom 1\noom_kill 1\n")
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=90)
        self.assertEqual(proc.returncode, 128 + signal.SIGTERM, stdout + stderr)
        record = self.records()[0]
        self.assertTrue(record["interrupted"])
        self.assertEqual(record["signals"], ["SIGTERM"])
        self.assertEqual((record["oom_killed"], record["oom_killed_in"], record["memory"]["oom_kill"]),
                         (True, self.own, 1))
        self.assertIn(f"interrupted=SIGTERM oom_killed={self.own}", Path(record["log"]).read_text())
        self.assertIn("test-tools: FAIL the kernel's OOM killer stopped this run", stderr)

    def test_an_interrupted_run_with_no_oom_kill_stays_a_plain_signal(self):
        self.fake(headroom=3 * GIB)
        proc = self.start_slow()
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=90)
        record = self.records()[0]
        self.assertEqual((record["interrupted"], record["oom_killed"], record["oom_killed_in"]), (True, False, None))
        self.assertNotIn("oom_killed", Path(record["log"]).read_text())
        self.assertNotIn("OOM", stderr)

    def test_a_kill_in_the_slice_explains_the_sigterm_of_a_run_in_another_scope(self):
        self.fake(headroom=3 * GIB)
        slice_events = self.cgroups / str(Path(self.own).parent).lstrip("/") / "memory.events"
        proc = self.start_slow()
        slice_events.write_text("low 0\nhigh 0\nmax 0\noom 1\noom_kill 1\n")
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=90)
        record = self.records()[0]
        self.assertEqual((record["oom_killed"], record["oom_killed_in"]), (True, str(Path(self.own).parent)))
        self.assertEqual((record["memory"]["oom_kill"], record["memory"]["limit_oom_kill"]), (0, 1))

    def test_pytest_killed_by_the_kernel_while_the_slice_saw_an_oom_kill_is_recorded_as_oom_killed(self):
        # The kernel's victim is pytest itself: SIGKILL, with no signal sent to the runner.
        self.fake(headroom=3 * GIB)
        slice_events = self.cgroups / str(Path(self.own).parent).lstrip("/") / "memory.events"
        proc = self.start_slow()
        slice_events.write_text("low 0\nhigh 0\nmax 0\noom 1\noom_kill 1\n")
        os.kill(int(self.marker.read_text()), signal.SIGKILL)
        stdout, stderr = proc.communicate(timeout=90)
        self.assertEqual(proc.returncode, 128 + signal.SIGKILL, stdout + stderr)
        record = self.records()[0]
        self.assertEqual((record["interrupted"], record["pytest_rc"]), (False, -signal.SIGKILL))
        self.assertEqual((record["oom_killed"], record["oom_killed_in"]), (True, str(Path(self.own).parent)))
        self.assertIn(f"oom_killed={Path(self.own).parent}", Path(record["log"]).read_text())

    # The worker count.

    def enough_for(self, workers: int) -> int:
        m = self.module
        return m.MEMORY_BASE_BYTES + workers * m.MEMORY_PER_WORKER_BYTES + m.MEMORY_PER_WORKER_BYTES // 2

    @unittest.skipIf(importlib.util.find_spec("xdist") is None, "pytest-xdist is not installed")
    def test_the_default_worker_count_is_what_the_slice_has_room_for(self):
        self.fake(headroom=self.enough_for(2))
        result = self.run_script(NEVR_TEST_WORKERS=None, PYTEST_XDIST_AUTO_NUM_WORKERS="8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for record in self.records("start") + self.records():
            self.assertEqual((record["mode"], record["workers"], record["workers_source"]),
                             ("parallel", 2, "default"))
            basis = record["workers_basis"]
            self.assertEqual((basis["limited_by"], basis["cpus"], basis["memory_workers"]), ("memory", 8, 2))
            self.assertEqual(basis["limit_cgroup"], str(Path(self.own).parent))
            self.assertEqual(record["cmd"][record["cmd"].index("-n") + 1], "2")
        self.assertIn("test-tools: worker basis ", Path(self.records()[0]["log"]).read_text())

    @unittest.skipIf(importlib.util.find_spec("xdist") is None, "pytest-xdist is not installed")
    def test_with_room_to_spare_the_cpus_decide(self):
        self.fake(headroom=self.enough_for(64))
        result = self.run_script(NEVR_TEST_WORKERS=None, PYTEST_XDIST_AUTO_NUM_WORKERS="3")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = self.records()[0]
        self.assertEqual((record["workers"], record["workers_basis"]["limited_by"]), (3, "cpu"))

    @unittest.skipIf(importlib.util.find_spec("xdist") is None, "pytest-xdist is not installed")
    def test_with_no_cgroup_to_read_the_cpus_decide_and_the_basis_says_why(self):
        result = self.run_script(NEVR_TEST_WORKERS=None, PYTEST_XDIST_AUTO_NUM_WORKERS="3")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        basis = self.records()[0]["workers_basis"]
        self.assertEqual((self.records()[0]["workers"], basis["limited_by"]), (3, "cpu"))
        self.assertIn("no memory limit", basis["memory"])

    @unittest.skipIf(importlib.util.find_spec("xdist") is None, "pytest-xdist is not installed")
    def test_an_explicit_worker_count_wins_and_is_recorded_as_given(self):
        self.fake(headroom=self.module.MEMORY_BASE_BYTES // 2)  # room for nothing
        cases = (  # (arguments, environment, workers, source)
            ([], {"NEVR_TEST_WORKERS": "3"}, 3, "NEVR_TEST_WORKERS"),
            (["-n", "2"], {"NEVR_TEST_WORKERS": "3"}, 2, "-n in the arguments"),
            (["-n", "2"], {"NEVR_TEST_WORKERS": None}, 2, "-n in the arguments"),
        )
        for i, (given, env, workers, source) in enumerate(cases):
            with self.subTest(arguments=given, environment=env):
                self.logs = self.root / f"logs{i}"
                result = self.run_script("-q", "-p", "no:cacheprovider", *given, str(self.suite), **env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for record in self.records("start") + self.records():
                    self.assertEqual((record["workers"], record["workers_source"], record["workers_basis"]),
                                     (workers, source, None))

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
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for record in self.records("start") + self.records():
            lock = record["lock"]
            self.assertEqual((lock["state"], lock["path"], lock["contended"]),
                             ("acquired", str(self.logs / "run.lock"), False))
            self.assertLess(lock["wait_s"], 5)
            self.assertEqual(lock["deadline_s"], 5400)

    def test_a_run_that_cannot_get_the_lock_by_its_deadline_is_refused_and_says_who_holds_it(self):
        self.hold_lock({"pid": 4242, "checkout": "/the/other/checkout"})
        result = self.run_script(NEVR_TEST_LOCK_WAIT_S="0.5")
        self.assertEqual(result.returncode, 75, result.stdout + result.stderr)
        self.assertIn("test-tools: FAIL the machine-wide test lock", result.stderr)
        self.assertIn("4242", result.stderr)
        self.assertIn("/the/other/checkout", result.stderr)
        self.assertNotIn("passed", result.stdout)
        self.assertNotIn("Traceback", result.stderr)
        self.assertEqual(self.records("start"), [])
        refused = self.records("lock_timeout")
        self.assertEqual(len(refused), 1)
        self.assertEqual((refused[0]["lock"]["state"], refused[0]["lock"]["deadline_s"]), ("timeout", 0.5))
        self.assertGreaterEqual(refused[0]["lock"]["wait_s"], 0.5)

    def test_a_run_waits_for_the_lock_and_records_how_long(self):
        fd = self.hold_lock({"pid": 4242})
        proc = subprocess.Popen([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(self.suite)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=REPO,
                                env=self.env(NEVR_TEST_LOCK_WAIT_S="60"))
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        waiting = proc.stderr.readline()  # written before it blocks, so the lock is released only after it
        self.assertIn("test-tools: waiting for the machine-wide test lock", waiting)
        self.assertIn("4242", waiting)
        fcntl.flock(fd, fcntl.LOCK_UN)
        stdout, stderr = proc.communicate(timeout=120)
        self.assertEqual(proc.returncode, 0, stdout + stderr)
        lock = self.records("start")[0]["lock"]
        self.assertEqual((lock["state"], lock["deadline_s"], lock["contended"]), ("acquired", 60, True))
        self.assertGreater(lock["wait_s"], 0)

    def test_the_lock_is_held_during_the_run_and_pytest_does_not_inherit_it(self):
        probe = self.write_suite("probe", LOCK_PROBE_SUITE)
        result = self.run_script("-q", "-p", "no:cacheprovider", str(probe),
                                 RUN_TOOL_TESTS_LOCK=str(self.logs / "run.lock"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_the_lock_is_released_when_the_run_ends(self):
        self.assertEqual(self.run_script().returncode, 0)
        fd = os.open(self.logs / "run.lock", os.O_RDWR)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)  # raises when the finished run still holds it
        self.assertEqual((self.logs / "run.lock").read_text(), "")  # and the file names no holder

    def test_each_log_directory_has_a_lock_of_its_own(self):
        # A run a test starts keeps its records in a directory of its own, so it never waits for the run that
        # started the test.
        self.hold_lock()
        other = self.root / "other-logs"
        result = subprocess.run([sys.executable, str(SCRIPT), "-q", "-p", "no:cacheprovider", str(self.suite)],
                                capture_output=True, text=True, cwd=REPO, timeout=120,
                                env=self.env(NEVR_TEST_LOG_DIR=str(other), NEVR_TEST_LOCK_WAIT_S="0.5"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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
