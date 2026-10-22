"""tools/scenario/smoke.sh brings its own worktree to origin/main before it does anything else, and
fails loudly (record plus alarm) when it cannot, so the timer never runs a dev branch (#792)."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SMOKE = REPO / "tools/scenario/smoke.sh"


FAKE_RUNNER = """import json, os, pathlib, sys
out = pathlib.Path(sys.argv[sys.argv.index("--out") + 1])
out.mkdir(parents=True, exist_ok=True)
seconds = float(os.environ["FAKE_IN_GAME_SECONDS"])
passed = os.environ["FAKE_PASSED"] == "1"
steps = [{"step": "client reached in game (multiplayer entry)", "result": "PASS" if passed else "FAIL",
          "seconds": seconds, "detail": "" if passed else "no match after 600 s"}]
report = {"scenario": "smoke", "passed": passed, "dll_restored": True, "loadavg_start": "1.00 2.00 3.00",
          "states": [{"from": "logged in", "to": "loading global", "ts": "2026-10-22T09:21:18.399Z"}], "steps": steps}
(out / "result.json").write_text(json.dumps(report))
sys.exit(0 if passed else 1)
"""


def git(cwd, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false",
                           *args], cwd=cwd, capture_output=True, text=True, check=True).stdout.strip()


class SmokeSyncTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.origin = self.tmp / "origin.git"
        subprocess.run(["git", "init", "--bare", "-b", "main", str(self.origin)], check=True, capture_output=True)
        self.seed = self.tmp / "seed"
        subprocess.run(["git", "clone", "-q", str(self.origin), str(self.seed)], check=True, capture_output=True)
        git(self.seed, "checkout", "-q", "-b", "main")
        (self.seed / "tools/scenario").mkdir(parents=True)
        shutil.copy(SMOKE, self.seed / "tools/scenario/smoke.sh")
        (self.seed / "marker.txt").write_text("one\n")
        # A stand-in runner: writes the result.json the real one writes, with what the test asks for.
        (self.seed / "tools/scenario/run_scenario.py").write_text(FAKE_RUNNER)
        (self.seed / "tools/scenario/smoke.yaml").write_text("name: smoke\n")
        (self.seed / "build/mingw-scenario/bin").mkdir(parents=True)
        (self.seed / "build/mingw-scenario/bin/BugSplat64.dll").write_text("dll")
        git(self.seed, "add", "-A")
        git(self.seed, "commit", "-q", "-m", "one")
        git(self.seed, "push", "-q", "origin", "main")
        self.tree = self.tmp / "smoke-tree"
        subprocess.run(["git", "clone", "-q", "-b", "main", str(self.origin), str(self.tree)], check=True, capture_output=True)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.alarms = self.tmp / "alarms.log"
        for tool in ("bonk", "courier"):
            fake = self.bin / tool
            fake.write_text(f'#!/bin/sh\necho "{tool} $*" >> "{self.alarms}"\n')
            fake.chmod(0o755)
        self.status = self.tmp / "status"
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", NEVR_SMOKE_STATUS_DIR=str(self.status))

    def push_commit(self, text="two\n", name="marker.txt"):
        (self.seed / name).write_text(text)
        git(self.seed, "add", "-A")
        git(self.seed, "commit", "-q", "-m", "next")
        git(self.seed, "push", "-q", "origin", "main")
        return git(self.seed, "rev-parse", "HEAD")

    def run_smoke(self, *args):
        return subprocess.run([str(self.tree / "tools/scenario/smoke.sh"), *args], env=self.env,
                              capture_output=True, text=True, timeout=60)

    def record(self):
        return json.loads((self.status / "last.json").read_text())

    def assert_loud(self, result, fragment):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(fragment, result.stderr)
        self.assertEqual(self.record()["verdict"], "FAIL")
        self.assertIn(fragment, self.record()["reason"])
        alarms = self.alarms.read_text()
        self.assertIn("bonk --from scenario-smoke NEVR scenario smoke FAILED", alarms)
        self.assertIn("courier send --to spritz --from scenario-smoke", alarms)

    def test_a_tree_behind_origin_is_fast_forwarded(self):
        tip = self.push_commit()
        result = self.run_smoke("--sync-only")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(git(self.tree, "rev-parse", "HEAD"), tip)
        self.assertFalse(self.alarms.exists())

    def test_the_run_continues_from_the_updated_file_not_the_old_one(self):
        # The pushed smoke.sh stops right before the lock wait with a marker; the old file in the
        # tree must hand over to it (re-exec) rather than carry on from its own text.
        new = (SMOKE.read_text()).replace("\nwait_for_lock\n", "\necho UPDATED-FILE-RAN; exit 0\n", 1)
        self.assertIn("UPDATED-FILE-RAN", new)
        (self.seed / "tools/scenario/smoke.sh").write_text(new)
        git(self.seed, "add", "-A")
        git(self.seed, "commit", "-q", "-m", "update smoke.sh")
        git(self.seed, "push", "-q", "origin", "main")
        result = self.run_smoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("UPDATED-FILE-RAN", result.stdout)

    def test_a_dirty_tree_fails_loudly_and_is_not_touched(self):
        self.push_commit()
        before = git(self.tree, "rev-parse", "HEAD")
        (self.tree / "marker.txt").write_text("local edit\n")
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "uncommitted changes")
        self.assertEqual(git(self.tree, "rev-parse", "HEAD"), before)

    def test_a_diverged_tree_fails_loudly(self):
        self.push_commit()
        (self.tree / "local.txt").write_text("local\n")
        git(self.tree, "add", "-A")
        git(self.tree, "commit", "-q", "-m", "local commit")
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "cannot fast-forward")

    def test_an_origin_that_cannot_be_reached_fails_loudly(self):
        shutil.rmtree(self.origin)
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "cannot fetch origin/main")

    def test_a_dry_run_alarm_records_the_failure_and_fires_nothing(self):
        (self.tree / "marker.txt").write_text("local edit\n")
        result = self.run_smoke("--sync-only", "--dry-run-alarm")
        self.assertEqual(result.returncode, 1)
        self.assertIn("[dry-run] alarm would fire", result.stderr)
        self.assertFalse(self.alarms.exists())
        self.assertEqual(self.record()["verdict"], "FAIL")


    def run_the_smoke(self, seconds, passed):
        self.env.update(FAKE_IN_GAME_SECONDS=str(seconds), FAKE_PASSED="1" if passed else "0",
                        NEVR_LAUNCH_LOCK=str(self.tmp / "no-such-lock"))
        return self.run_smoke("--no-build")

    def test_a_fast_pass_is_a_plain_pass_with_the_timings_recorded(self):
        result = self.run_the_smoke(250.0, True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = self.record()
        self.assertEqual((record["verdict"], record["slow"], record["in_game_seconds"]), ("PASS", False, 250.0))
        self.assertEqual(record["loadavg_start"], "1.00 2.00 3.00")
        self.assertEqual(record["states"][0]["to"], "loading global")
        self.assertFalse(self.alarms.exists())

    def test_a_pass_over_300_s_is_still_a_pass_flagged_slow_and_raises_no_alarm(self):
        result = self.run_the_smoke(400.0, True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.record()["verdict"], self.record()["slow"]), ("PASS", True))
        self.assertIn("SLOW: in game after 400.0 s", (self.status / "last.txt").read_text())
        self.assertFalse(self.alarms.exists())

    def test_a_failed_run_alarms_and_keeps_the_timings(self):
        result = self.run_the_smoke(600.0, False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.record()
        self.assertEqual((record["verdict"], record["exit"]), ("FAIL", 1))
        self.assertIn("client reached in game", record["reason"])
        self.assertEqual(record["loadavg_start"], "1.00 2.00 3.00")
        self.assertIn("bonk --from scenario-smoke NEVR scenario smoke FAILED", self.alarms.read_text())


class SmokeUnitTest(unittest.TestCase):
    """The unit runs smoke.sh from the worktree that tracks origin/main, inside the build slice."""
    unit = (REPO / "tools/scenario/systemd/nevr-scenario-smoke.service").read_text()

    def test_it_runs_from_the_dedicated_main_tracking_worktree(self):
        wt = "/home/andrew/src/nevr-runtime/.claude/worktrees/nevr-scenario-smoke"
        self.assertIn(f"WorkingDirectory={wt}\n", self.unit)
        self.assertIn(f"ExecStart={wt}/tools/scenario/smoke.sh\n", self.unit)
        self.assertNotIn("spritz-scenario-smoke", self.unit)

    def test_it_builds_inside_the_shared_build_slice_and_off_the_wayland_display(self):
        self.assertIn("Slice=nevr-builds.slice\n", self.unit)
        self.assertIn("MemoryMax=8G\n", self.unit)
        self.assertIn("UnsetEnvironment=WAYLAND_DISPLAY\n", self.unit)


if __name__ == "__main__":
    unittest.main()
