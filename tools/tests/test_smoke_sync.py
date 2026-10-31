"""tools/scenario/smoke.sh brings its own worktree to origin/main before it does anything else, and
fails loudly (record plus a non-zero exit) when it cannot, so a scheduled run never uses a dev branch (#792)."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import time
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
        (self.seed / "tools/lib").mkdir(parents=True)
        shutil.copy(REPO / "tools/lib/work_root.sh", self.seed / "tools/lib/work_root.sh")
        (self.seed / "marker.txt").write_text("one\n")
        # A stand-in runner: writes the result.json the real one writes, with what the test asks for.
        (self.seed / "tools/scenario/run_scenario.py").write_text(FAKE_RUNNER)
        (self.seed / "tools/scenario/smoke.yaml").write_text("name: smoke\n")
        (self.seed / "build/mingw-scenario/bin").mkdir(parents=True)
        (self.seed / "build/mingw-scenario/bin/BugSplat64.dll").write_text("dll")
        (self.seed / "build/mingw-scenario/bin/nevr-runtime.dll").write_text("dll")
        git(self.seed, "add", "-A")
        git(self.seed, "commit", "-q", "-m", "one")
        git(self.seed, "push", "-q", "origin", "main")
        self.tree = self.tmp / "smoke-tree"
        subprocess.run(["git", "clone", "-q", "-b", "main", str(self.origin), str(self.tree)], check=True, capture_output=True)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.status = self.tmp / "status"
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", NEVR_SMOKE_STATUS_DIR=str(self.status),
                        NEVR_SMOKE_BRANCH="main")

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

    def test_a_tree_behind_origin_is_fast_forwarded(self):
        tip = self.push_commit()
        result = self.run_smoke("--sync-only")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(git(self.tree, "rev-parse", "HEAD"), tip)

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

    def commit_locally(self):
        (self.tree / "local.txt").write_text("local\n")
        git(self.tree, "add", "-A")
        git(self.tree, "commit", "-q", "-m", "local commit")
        return git(self.tree, "rev-parse", "HEAD")

    def test_a_tree_ahead_of_origin_main_is_a_dev_tree_and_fails_loudly(self):
        dev = self.commit_locally()
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "not at origin/main after the fast-forward")
        self.assertEqual(git(self.tree, "rev-parse", "HEAD"), dev)

    def test_a_detached_head_at_a_dev_commit_fails_loudly(self):
        self.commit_locally()
        git(self.tree, "checkout", "-q", "--detach")
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "a detached HEAD, not main")

    def test_a_detached_head_behind_origin_main_fails_loudly_and_is_not_moved(self):
        self.push_commit()
        git(self.tree, "checkout", "-q", "--detach")
        before = git(self.tree, "rev-parse", "HEAD")
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "a detached HEAD, not main")
        self.assertEqual(git(self.tree, "rev-parse", "HEAD"), before)

    def test_another_branch_with_a_dev_commit_fails_loudly(self):
        git(self.tree, "checkout", "-q", "-b", "dev")
        self.commit_locally()
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "on dev, not main")

    def test_another_branch_behind_origin_main_fails_loudly_and_its_ref_is_not_moved(self):
        self.push_commit()
        git(self.tree, "checkout", "-q", "-b", "dev")
        before = git(self.tree, "rev-parse", "dev")
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "on dev, not main")
        self.assertEqual(git(self.tree, "rev-parse", "dev"), before)

    def test_an_untracked_file_makes_the_tree_dirty(self):
        (self.tree / "stray.txt").write_text("x\n")
        result = self.run_smoke("--sync-only")
        self.assert_loud(result, "uncommitted changes")

    def test_a_fetch_that_hangs_fails_loudly_at_the_timeout(self):
        real_git = shutil.which("git")
        shim = self.bin / "git"
        shim.write_text(f'#!/bin/sh\ncase " $* " in *" fetch "*) sleep 30; exit 0;; esac\nexec {real_git} "$@"\n')
        shim.chmod(0o755)
        self.env["SMOKE_FETCH_TIMEOUT_SECONDS"] = "1"
        started = time.monotonic()
        result = self.run_smoke("--sync-only")
        self.assertLess(time.monotonic() - started, 20)
        self.assert_loud(result, "cannot fetch origin/main for")
        self.assertIn("within 1 s", result.stderr)

    def test_the_record_defaults_to_the_work_root_with_a_readme(self):
        # No NEVR_SMOKE_STATUS_DIR: the status directory is under NEVR_WORK_ROOT, and carries a README.
        root = self.tmp / "work"
        env = dict(self.env, NEVR_WORK_ROOT=str(root), HOME=str(self.tmp / "home"))
        env.pop("NEVR_SMOKE_STATUS_DIR")
        (self.tree / "marker.txt").write_text("local edit\n")
        result = subprocess.run([str(self.tree / "tools/scenario/smoke.sh"), "--sync-only"], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(json.loads((root / "scenario-smoke/last.json").read_text())["verdict"], "FAIL")
        self.assertIn("smoke.sh", (root / "scenario-smoke/README.md").read_text())

    def test_the_game_run_lock_defaults_to_the_work_root(self):
        # The smoke waits on the lock launch-client.sh takes; with no NEVR_LAUNCH_LOCK that is the work root's.
        import fcntl
        root = self.tmp / "work"
        root.mkdir()
        env = dict(self.env, NEVR_WORK_ROOT=str(root), HOME=str(self.tmp / "home"), SMOKE_LOCK_WAIT_SECONDS="1")
        env.pop("NEVR_LAUNCH_LOCK", None)
        with open(root / "launch-client.lock", "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            result = subprocess.run([str(self.tree / "tools/scenario/smoke.sh"), "--no-sync", "--no-build"], env=env,
                                    capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(f"game-run lock {root}/launch-client.lock still busy", result.stderr)

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

    def test_exactly_300_s_is_not_slow(self):
        result = self.run_the_smoke(300.0, True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.record()["slow"])

    def test_a_pass_over_300_s_is_still_a_pass_flagged_slow(self):
        result = self.run_the_smoke(400.0, True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.record()["verdict"], self.record()["slow"]), ("PASS", True))
        self.assertIn("SLOW: in game after 400.0 s", (self.status / "last.txt").read_text())

    def test_a_failed_run_fails_and_keeps_the_timings(self):
        result = self.run_the_smoke(600.0, False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        record = self.record()
        self.assertEqual((record["verdict"], record["exit"]), ("FAIL", 1))
        self.assertIn("client reached in game", record["reason"])
        self.assertEqual(record["loadavg_start"], "1.00 2.00 3.00")


if __name__ == "__main__":
    unittest.main()
