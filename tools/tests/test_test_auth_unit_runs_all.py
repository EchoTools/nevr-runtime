"""`just test-auth-unit` runs every test binary and fails once at the end, naming each failure (#1228).

The recipe's own `run_test` function and its closing summary are run here, against stand-in binaries and a
stand-in `wine`, so the check drives the text that ships. A recipe that exits at the first failing binary
leaves the later ones unrun, and the first test below fails.
"""
import os
import pathlib
import re
import stat
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]

# Like Wine running a GTest binary: a passing binary writes its XML result where --gtest_output names it. A name with
# "silent" exits 0 and writes nothing (a crash that exits 0, #2152); "xmlcount" writes a result with one failure.
FAKE_WINE = """#!/usr/bin/env bash
echo "ran $1"
case "$1" in *fail*) exit 1 ;; esac
case "$1" in *silent*) exit 0 ;; esac
xml=""
for arg in "$@"; do case "$arg" in --gtest_output=xml:*) xml="${arg#--gtest_output=xml:}" ;; esac; done
failures=0
case "$1" in *xmlcount*) failures=1 ;; esac
if [ -n "$xml" ]; then
  printf '<?xml version="1.0" encoding="UTF-8"?>\n<testsuites tests="2" failures="%s" disabled="0" errors="0" time="0.001" name="AllTests"></testsuites>\n' "$failures" > "$xml"
fi
exit 0
"""

# Makes `systemd-run --user --scope` unavailable, so run_test takes its time-limit-only branch.
FAKE_SYSTEMD_RUN = "#!/usr/bin/env bash\nexit 1\n"


def recipe_text():
    body = just_recipe.body(just_recipe.read_justfile(), "test-auth-unit")
    if body is None:
        raise AssertionError("justfile no longer has a test-auth-unit recipe")
    return body


def run_test_function(body):
    match = re.search(r"^    run_test\(\) \{\n.*?^    \}\n", body, re.M | re.S)
    if match is None:
        raise AssertionError("test-auth-unit no longer defines run_test")
    return match.group(0)


def failure_summary(body):
    match = re.search(r'^    if \[\[ "\$\{#test_failures\[@\]\}" -gt 0 \]\]; then\n.*?^    fi\n', body, re.M | re.S)
    if match is None:
        raise AssertionError("test-auth-unit no longer ends with the failure summary (#1228)")
    return match.group(0)


class TestAuthUnitRunsEveryBinaryTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        tmp = pathlib.Path(self._tmp.name)
        self.bin = tmp / "bin"
        self.bin.mkdir()
        self._write_exec(self.bin / "wine", FAKE_WINE)
        self._write_exec(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}")
        self.tmp = tmp
        self.script = tmp / "run.sh"
        self.names = ["first_fail.exe", "second_ok.exe", "third_fail.exe", "fourth_ok.exe"]
        self.write_script(self.names)

    def write_script(self, names):
        """The recipe's function and summary, then one run_test per binary (named in the tree), in the recipe's order."""
        body = recipe_text()
        self.script.write_text(
            "set -euo pipefail\n"
            "test_failures=()\n"
            + run_test_function(body)
            + "".join(f'run_test "{self.tmp / name}"\n' for name in names)
            + failure_summary(body)
            + 'echo "REACHED END"\n'
        )

    @staticmethod
    def _write_exec(path, text):
        path.write_text(text)
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def run_script(self):
        # The recipe runs from the repository root, where tools/gtest_verdict.py is found.
        return subprocess.run(["bash", str(self.script)], env=self.env, cwd=REPO,
                              capture_output=True, text=True, timeout=60)

    def test_a_failing_first_binary_does_not_stop_the_later_ones(self):
        result = self.run_script()
        for name in ("first_fail.exe", "second_ok.exe", "third_fail.exe", "fourth_ok.exe"):
            self.assertIn(f"ran {self.tmp / name}", result.stdout, result.stdout + result.stderr)

    def test_the_recipe_fails_once_at_the_end_and_names_every_failing_binary(self):
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("test-auth-unit: FAIL — 2 test binary(ies) failed:", result.stderr)
        self.assertIn("first_fail.exe (exit 1)", result.stderr)
        self.assertIn("third_fail.exe (exit 1)", result.stderr)
        self.assertNotIn("REACHED END", result.stdout)

    def test_a_run_with_no_failures_passes_through_the_summary(self):
        self.write_script(["second_ok.exe", "fourth_ok.exe"])
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("REACHED END", result.stdout)

    def test_a_zero_exit_with_no_gtest_result_is_a_failure(self):
        # Under Wine a stack overflow exited 0 with no GTest summary (#2152): the exit code alone is not a pass.
        self.write_script(["second_ok.exe", "silent_crash.exe", "fourth_ok.exe"])
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("silent_crash.exe (no GTest result, exit 0)", result.stderr)
        self.assertIn("result unreadable", result.stderr)
        self.assertNotIn("REACHED END", result.stdout)

    def test_a_result_left_by_an_earlier_run_is_not_a_pass(self):
        # run_test removes the XML before the run, so a passing result from an earlier run cannot stand in for this one.
        stale = self.tmp / "silent_crash.exe.gtest.xml"
        stale.write_text('<testsuites tests="2" failures="0" errors="0" name="AllTests"/>\n', encoding="utf-8")
        self.write_script(["silent_crash.exe"])
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(stale.exists())

    def test_a_gtest_result_with_a_failure_is_a_failure_even_at_exit_zero(self):
        self.write_script(["second_ok.exe", "xmlcount.exe"])
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("xmlcount.exe (no GTest result, exit 0)", result.stderr)
        self.assertIn("1 failed, 0 errors of 2 tests", result.stderr)

    def test_the_recipe_does_not_exit_inside_run_test(self):
        # Mutant guard: the early `exit "$rc"` that stopped the loop must not come back.
        self.assertNotIn("exit \"$rc\"", run_test_function(recipe_text()))


if __name__ == "__main__":
    unittest.main()
