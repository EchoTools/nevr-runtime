"""tools/verify-parallel.sh runs the suites side by side and fails closed, naming what failed (#794)."""
import fcntl
import os
import pathlib
import sys
import time
import shutil
import subprocess
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/verify-parallel.sh"

# A stand-in for `just <recipe>`: the recipe's outcome comes from the file named after it in
# $FAKE_DIR/<recipe>.rc (default 0), after an optional barrier that only passes when another
# recipe is running at the same time.
FAKE_JUST = """#!/usr/bin/env bash
recipe="$1"
echo "output of $recipe"
touch "$FAKE_DIR/$recipe.started"
if [[ -f "$FAKE_DIR/$recipe.waits_for" ]]; then
  other="$(cat "$FAKE_DIR/$recipe.waits_for")"
  for _ in $(seq 1 100); do [[ -f "$FAKE_DIR/$other.started" ]] && break; sleep 0.1; done
  [[ -f "$FAKE_DIR/$other.started" ]] || { echo "never saw $other running" >&2; exit 1; }
fi
rc=0; [[ -f "$FAKE_DIR/$recipe.rc" ]] && rc="$(cat "$FAKE_DIR/$recipe.rc")"
exit "$rc"
"""


class VerifyParallelTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        tmp = pathlib.Path(self._tmp.name)
        self.repo = tmp / "repo"
        (self.repo / "tools").mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        shutil.copy(SCRIPT, self.repo / "tools/verify-parallel.sh")
        self.bin = tmp / "bin"
        self.bin.mkdir()
        (self.bin / "just").write_text(FAKE_JUST)
        (self.bin / "just").chmod(0o755)
        self.fake = tmp / "fake"
        self.fake.mkdir()
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", FAKE_DIR=str(self.fake))

    def run_script(self, *suites):
        return subprocess.run(["bash", str(self.repo / "tools/verify-parallel.sh"), *suites], env=self.env,
                              cwd=self.repo, capture_output=True, text=True, timeout=60)

    def test_all_suites_passing_exits_zero_and_prints_every_log(self):
        result = self.run_script("a", "b", "c")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify: all 3 suites passed", result.stdout)
        for suite in "abc":
            self.assertIn(f"output of {suite}", result.stdout)
            self.assertIn(f"[{suite}] PASS", result.stdout)

    def test_one_failing_suite_fails_the_block_and_is_named(self):
        (self.fake / "b.rc").write_text("1")
        result = self.run_script("a", "b", "c")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("verify: FAIL — 1 of 3 suite(s) failed: b", result.stderr)
        self.assertIn("[b] FAIL (exit 1", result.stdout)
        self.assertTrue((self.fake / "a.started").exists() and (self.fake / "c.started").exists())

    def test_every_failing_suite_is_named_in_argument_order(self):
        (self.fake / "a.rc").write_text("1")
        (self.fake / "c.rc").write_text("1")
        result = self.run_script("a", "b", "c")
        self.assertEqual(result.returncode, 1)
        self.assertIn("2 of 3 suite(s) failed: a c", result.stderr)

    def test_a_suite_exit_code_other_than_one_still_fails_the_block(self):
        (self.fake / "a.rc").write_text("123")
        result = self.run_script("a", "b")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("[a] FAIL (exit 123", result.stdout)

    def test_no_suites_is_a_usage_error(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 2)
        self.assertIn("no suite recipes given", result.stderr)

    def test_a_repeated_suite_name_is_refused_before_anything_runs(self):
        # Results are keyed by suite name, so a repeat would overwrite a failure with a pass.
        result = self.run_script("a", "b", "a")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("duplicate suite recipe: a", result.stderr)
        self.assertFalse((self.fake / "a.started").exists() or (self.fake / "b.started").exists())

    @unittest.skipIf((os.cpu_count() or 1) < 2, "needs two cores to run two suites at once")
    def test_suites_run_at_the_same_time_not_one_after_another(self):
        # Each suite waits until it has seen the other running; one after another, the first times out.
        (self.fake / "a.waits_for").write_text("b")
        (self.fake / "b.waits_for").write_text("a")
        result = self.run_script("a", "b")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)



class VcpkgQuestHostGuardTest(unittest.TestCase):
    """`_vcpkg-quest-host` installs yaml-cpp once and is a no-op while the install is current, so the
    parallel suites that depend on it never run vcpkg at the same time (#794)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)
        shutil.copy(REPO / "justfile", self.root / "justfile")
        (self.root / "tools/quest-host-deps").mkdir(parents=True)
        self.manifest = self.root / "tools/quest-host-deps/vcpkg.json"
        self.manifest.write_text("{}")
        self.lib = self.root / "build/quest-host-vcpkg/x64-linux/lib/libyaml-cpp.a"
        self.lib.parent.mkdir(parents=True)
        self.header = self.root / "build/quest-host-vcpkg/x64-linux/include/nlohmann/json.hpp"
        self.header.parent.mkdir(parents=True)
        self.calls = self.root / "vcpkg-calls"
        vcpkg_dir = self.root / "fake-vcpkg-root"
        vcpkg_dir.mkdir()
        (vcpkg_dir / "vcpkg").write_text(f'#!/bin/sh\necho "$@" >> "{self.calls}"\n')
        (vcpkg_dir / "vcpkg").chmod(0o755)
        (self.root / "tools/vcpkg_root.sh").write_text(f'#!/bin/sh\necho "{vcpkg_dir}"\n')
        (self.root / "tools/vcpkg_root.sh").chmod(0o755)

    def run_recipe(self):
        return subprocess.run(["just", "--justfile", str(self.root / "justfile"), "_vcpkg-quest-host"],
                              cwd=self.root, capture_output=True, text=True, timeout=60)

    def calls_made(self):
        return len(self.calls.read_text().splitlines()) if self.calls.exists() else 0

    def test_a_current_install_runs_no_vcpkg(self):
        self.lib.write_text("lib")
        self.header.write_text("json")
        os.utime(self.manifest, (1_000_000, 1_000_000))
        result = self.run_recipe()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls_made(), 0)

    def test_a_missing_install_runs_vcpkg_once(self):
        result = self.run_recipe()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls_made(), 1)
        self.assertIn("--x-install-root=" + str(self.root / "build/quest-host-vcpkg"), self.calls.read_text())

    def test_an_install_without_nlohmann_json_is_completed(self):
        # An install made from a manifest that did not name nlohmann-json has the library and not the header.
        self.lib.write_text("lib")
        os.utime(self.manifest, (1_000_000, 1_000_000))
        result = self.run_recipe()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls_made(), 1)

    def test_an_install_older_than_the_manifest_is_redone(self):
        self.lib.write_text("lib")
        self.header.write_text("json")
        os.utime(self.lib, (1_000_000, 1_000_000))
        result = self.run_recipe()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls_made(), 1)

    def test_a_second_install_waits_for_the_lock_and_then_sees_the_first_one_finished(self):
        (self.root / "build").mkdir(exist_ok=True)
        with open(self.root / "build/quest-host-vcpkg.install.lock", "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            proc = subprocess.Popen(["just", "--justfile", str(self.root / "justfile"), "_vcpkg-quest-host"],
                                    cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(1.0)
            self.assertIsNone(proc.poll(), "the recipe ran while another install held the lock")
            self.assertEqual(self.calls_made(), 0)
            # The first install finishes: its artifact appears, newer than the manifest, then it unlocks.
            self.lib.write_text("lib")
            self.header.write_text("json")
            fcntl.flock(held, fcntl.LOCK_UN)
            out, err = proc.communicate(timeout=30)
        self.assertEqual(proc.returncode, 0, out + err)
        self.assertEqual(self.calls_made(), 0, "the waiter ran vcpkg again after the first install finished")


class PreRunBeforeFanOutTest(unittest.TestCase):
    """`just verify` runs `just _vcpkg-quest-host` serially before tools/verify-parallel.sh starts the suites, so the
    first quest-host install is never one of the parallel workers (#1408). Pins the order; the recipe already had it."""

    QUEST_CONSUMERS = ("test-quest-data", "test-quest-data-net", "test-quest-shared", "test-quest-hooks",
                       "test-quest-redirect", "test-quest-integration")

    def verify_recipe(self):
        body = just_recipe.body(just_recipe.read_justfile(), "verify")
        self.assertIsNotNone(body, "justfile no longer has a verify recipe")
        return body

    def test_the_quest_host_install_runs_before_the_parallel_block(self):
        body = self.verify_recipe()
        pre = body.find("    just _vcpkg-quest-host\n")
        fan = body.find("bash tools/verify-parallel.sh")
        self.assertNotEqual(pre, -1, "verify no longer runs _vcpkg-quest-host before the parallel block")
        self.assertNotEqual(fan, -1, "verify no longer runs tools/verify-parallel.sh")
        self.assertLess(pre, fan, "_vcpkg-quest-host must run before the parallel suites start")

    def test_every_quest_host_consumer_is_in_the_parallel_block(self):
        # The fan-out is one command whose suite names continue with a trailing backslash; the first line
        # without one ends it. Compare whole tokens, so test-quest-data does not match test-quest-data-net.
        lines = self.verify_recipe().splitlines()
        start = next(i for i, line in enumerate(lines) if "bash tools/verify-parallel.sh" in line)
        end = start
        while lines[end].rstrip().endswith("\\"):
            end += 1
        tokens = " ".join(lines[start:end + 1]).replace("\\", " ").split()
        for suite in self.QUEST_CONSUMERS:
            self.assertIn(suite, tokens, f"{suite} is not in the verify fan-out, so it runs before or outside the pre-run")


if __name__ == "__main__":
    unittest.main()
