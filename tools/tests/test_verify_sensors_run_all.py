"""The sensor phase of `just verify` runs every sensor and names every failing one at the end (#1228).

The phase is the recipe's text from the "Sensor plumbing" comment to its last line. A sensor records its failure through
`sensor_fail` (one `verify: FAIL —` line, one entry in `verify_failed`); the one exit is the summary at the end.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
PLUMBING_MARKER = "# --- Sensor plumbing (N93)"
FAIL_PREFIX = "verify: FAIL — "
SUMMARY_PREFIX = "verify: FAILED — "
PASS_LINE_PREFIX = "verify: OK"
SYNTHETIC_PRE = "set -euo pipefail\n"
MIN_FAILING_SENSORS = 2  # `just go-tools` and a tool that reads the missing build: two different sensors
NO_PRESET = "no-such-preset"  # names a build directory that does not exist


def verify_phase_lines() -> list[str]:
    """The sensor phase of the verify recipe, de-indented, from the plumbing comment to the last line."""
    body = just_recipe.body(just_recipe.read_justfile(), "verify")
    assert body is not None, "justfile has no verify recipe"
    lines = textwrap.dedent(body).splitlines()
    start = next(i for i, line in enumerate(lines) if PLUMBING_MARKER in line)
    return lines[start:]


def code_lines(lines: list[str]) -> list[str]:
    """The lines that are not comment-only lines."""
    return [line for line in lines if not line.lstrip().startswith("#")]


class VerifySensorsRunAllTest(unittest.TestCase):
    def run_phase(self, snippet: str, cwd: pathlib.Path) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", "-c", SYNTHETIC_PRE + snippet], cwd=cwd, capture_output=True, text=True,
                              timeout=300)

    def test_no_sensor_exits_the_recipe_before_the_summary(self):
        exits = [line for line in code_lines(verify_phase_lines()) if re.fullmatch(r"\s*exit( \d+)?", line)]
        self.assertEqual(len(exits), 1, "the summary is the one exit of the sensor phase; found: " + repr(exits))

    def test_the_phase_defines_the_recorder_and_ends_with_the_summary(self):
        phase = "\n".join(verify_phase_lines())
        self.assertIn("verify_failed=()", phase)
        self.assertIn("sensor_fail() {", phase)
        self.assertIn(SUMMARY_PREFIX, phase)
        self.assertLess(phase.index(SUMMARY_PREFIX), phase.index(PASS_LINE_PREFIX))

    def test_two_failing_helpers_print_both_lines_and_the_summary_and_exit_non_zero(self):
        # The recorder and the two helpers that wrap it, taken from the recipe, then two failing sensors.
        phase = "\n".join(verify_phase_lines())
        helpers = phase[phase.index("verify_failed=()"):phase.index("# SIGPIPE + pipefail")]
        summary_start = phase.index('if [ "${#verify_failed[@]}" -gt 0 ]')
        summary = phase[summary_start:phase.index(PASS_LINE_PREFIX)]
        snippet = helpers + textwrap.dedent("""
            sensor_stage1 "first sensor" "a missing subject" 2
            sensor_nonempty "second sensor" "an extraction" ""
            echo "ran after both failures"
        """) + summary + "\necho verify: OK\n"
        with tempfile.TemporaryDirectory() as tmp:
            done = self.run_phase(snippet, pathlib.Path(tmp))
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn("ran after both failures", done.stdout)
        self.assertIn(f"{FAIL_PREFIX}sensor 'first sensor'", done.stderr)
        self.assertIn(f"{FAIL_PREFIX}sensor 'second sensor'", done.stderr)
        self.assertIn(f"{SUMMARY_PREFIX}2 sensor(s) failed", done.stderr)
        self.assertNotIn(PASS_LINE_PREFIX, done.stdout)

    def test_the_real_sensor_phase_on_this_tree_without_a_build_prints_each_failure_and_names_them_all(self):
        # The whole phase, run in this checkout with no build output: the tools that read the built DLLs fail, and a
        # stand-in `just` fails too, so the phase builds nothing. Every one of them must be reported, not only the first.
        # The tools test run is left out: it runs the whole `tools/tests` suite, this test included.
        phase = [line for line in verify_phase_lines() if "test-go-tools" not in line]
        snippet = "unset VCPKG_ROOT\n" + "\n".join(phase).replace("{{ preset }}", NO_PRESET)
        with tempfile.TemporaryDirectory() as tmp:
            fake_just = pathlib.Path(tmp) / "just"
            fake_just.write_text("#!/usr/bin/env bash\nexit 1\n")
            fake_just.chmod(0o755)
            env = dict(os.environ, PATH=f"{tmp}{os.pathsep}{os.environ['PATH']}")
            done = subprocess.run(["bash", "-c", SYNTHETIC_PRE + snippet], cwd=REPO, env=env, capture_output=True,
                                  text=True, timeout=300)
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        fails = [line for line in done.stderr.splitlines() if line.startswith(FAIL_PREFIX)]
        self.assertGreaterEqual(len(fails), MIN_FAILING_SENSORS, done.stderr)
        summary = next((line for line in done.stderr.splitlines() if line.startswith(SUMMARY_PREFIX)), None)
        self.assertIsNotNone(summary, "no summary line: the phase stopped before its end\n" + done.stderr)
        self.assertIn(f"{len(fails)} sensor(s) failed", summary)


if __name__ == "__main__":
    unittest.main()
