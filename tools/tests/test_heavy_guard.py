"""tools/heavy_guard.py and the _heavy-guard recipe: a heavy recipe runs from a person's terminal, or from a flagged run's
own recipes, and never two heavy runs at once.

These tests drive the real guard through a just file whose guard recipe is the one the repo's justfile carries
(extracted from the justfile, not copied), so a change to either file is what the tests see.
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GUARD = REPO / "tools" / "heavy_guard.py"
JUSTFILE = REPO / "justfile"
FLAG = "--i-know-exactly-what-im-doing"
REFUSAL = "{recipe} is a heavy build and is not run from a Claude session. Run the one test you changed instead."

# Every recipe that does a full configure+build of a tree, or runs a full test suite (see the guard's commit message).
HEAVY = [
    "verify", "build", "verbose-build", "dist", "verbose-dist", "build-android", "verbose-build-android",
    "test-android", "package-dev", "_quest-apk", "test-auth-unit", "test-auth",
    "test-system", "test-system-verbose", "scenario", "scenario-all", "test-plugins", "test-plugins-verbose",
]

FIXTURE_TAIL = '''
fake *args: (_heavy-guard "fake" args)
    @echo "fake body ran"

slow *args: (_heavy-guard "slow" args)
    @sleep 3
    @echo "slow body ran"

outer *args: (_heavy-guard "outer" args)
    @just --justfile "{{ justfile() }}" inner

inner *args: (_heavy-guard "inner" args)
    @echo "inner body ran"

[positional-arguments]
fwd *ARGS: (_heavy-guard "fwd" ARGS)
    #!/usr/bin/env bash
    set -euo pipefail
    raw="$(python3 -I tools/heavy_guard.py strip-optin "$@")"
    args=(); if [ -n "$raw" ]; then mapfile -t args <<<"$raw"; fi
    printf '[%s]\\n' "${args[@]}"
'''


def guard_recipe_text() -> str:
    """The _heavy-guard recipe as the repo's justfile carries it: its comment and attributes above, its body below."""
    lines = JUSTFILE.read_text().splitlines()
    start = next((i for i, line in enumerate(lines) if re.match(r"_heavy-guard\b", line)), None)
    if start is None:
        raise AssertionError("the justfile has no _heavy-guard recipe")
    first = start
    while first > 0 and (lines[first - 1].startswith("#") or lines[first - 1].startswith("[")):
        first -= 1
    end = start + 1
    while end < len(lines) and lines[end].startswith(" "):
        end += 1
    return "\n".join(lines[first:end]) + "\n"


class GuardFixture(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="heavy-guard-test-"))
        (self.tmp / "tools").mkdir()
        shutil.copy(GUARD, self.tmp / "tools" / "heavy_guard.py")
        (self.tmp / "justfile").write_text(guard_recipe_text() + FIXTURE_TAIL)
        self.env = {
            k: v
            for k, v in os.environ.items()
            if k not in ("CLAUDECODE", "CLAUDE_CODE_CHILD_SESSION", "NEVR_HEAVY_OK")
        }
        self.env["XDG_RUNTIME_DIR"] = str(self.tmp / "run")
        self.env["XDG_STATE_HOME"] = str(self.tmp / "state")
        os.makedirs(self.tmp / "run")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _argv(self, *args):
        return ["just", "--justfile", str(self.tmp / "justfile"), *args]

    def _env(self, claude, extra_env):
        env = dict(self.env)
        if claude:
            env["CLAUDECODE"] = "1"
        env.update(extra_env or {})
        return env

    def just(self, *args, claude=False, extra_env=None, timeout=60):
        return subprocess.run(
            self._argv(*args), cwd=self.tmp, env=self._env(claude, extra_env),
            capture_output=True, text=True, timeout=timeout,
        )

    def spawn(self, *args, claude=False, extra_env=None):
        return subprocess.Popen(
            self._argv(*args), cwd=self.tmp, env=self._env(claude, extra_env),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

    def state_records(self):
        path = self.tmp / "state" / "nevr-runtime" / "heavy-guard.jsonl"
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


class HeavyGuardTest(GuardFixture):
    def test_refused_from_claude_without_flag(self):
        r = self.just("fake", claude=True)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn(REFUSAL.format(recipe="fake"), r.stderr)
        self.assertNotIn("fake body ran", r.stdout)
        self.assertNotIn(FLAG, r.stdout + r.stderr, "the refusal must not name or hint at the opt-in")
        self.assertIn("refused", [rec["decision"] for rec in self.state_records()])

    def test_runs_from_claude_with_flag(self):
        r = self.just("fake", FLAG, claude=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("fake body ran", r.stdout)

    def test_runs_from_terminal_without_claude(self):
        r = self.just("fake")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("fake body ran", r.stdout)

    def test_env_vars_do_not_unlock(self):
        for extra in ({"NEVR_HEAVY_OK": "1"}, {"NEVR_HEAVY_OK": "1", "I_KNOW_EXACTLY_WHAT_IM_DOING": "1"}):
            with self.subTest(env=extra):
                r = self.just("fake", claude=True, extra_env=extra)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertNotIn("fake body ran", r.stdout)

    def test_second_run_waits_for_first(self):
        first = self.spawn("slow", FLAG, claude=True)
        time.sleep(1.0)
        started = time.monotonic()
        second = self.just("fake", timeout=120)
        elapsed = time.monotonic() - started
        first_out, _ = first.communicate(timeout=120)
        self.assertEqual(first.returncode, 0, first_out)
        self.assertIn("slow body ran", first_out)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("fake body ran", second.stdout)
        self.assertIn("waiting for", second.stdout + second.stderr)
        self.assertGreaterEqual(elapsed, 1.0, "the second run did not wait for the first")

    def test_nested_recipe_of_flagged_run_passes_without_waiting(self):
        r = self.just("outer", FLAG, claude=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("inner body ran", r.stdout)

    def test_forwarded_arguments_lose_the_opt_in(self):
        r = self.just("fwd", "a", "b c", FLAG, claude=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.splitlines(), ["[a]", "[b c]"])


class HeavyRecipesAreGatedTest(unittest.TestCase):
    def test_justfile_parses(self):
        r = subprocess.run(
            ["just", "--justfile", str(JUSTFILE), "--dump"], cwd=REPO, capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_each_heavy_recipe_depends_on_the_guard(self):
        text = JUSTFILE.read_text()
        for name in HEAVY:
            with self.subTest(recipe=name):
                pattern = rf'^{re.escape(name)} [^\n]*\(_heavy-guard "{re.escape(name)}" '
                self.assertRegex(text, re.compile(pattern, re.M))

    def test_flag_is_in_no_document(self):
        docs = [REPO / "AGENTS.md", REPO / "README.md"] + sorted((REPO / "docs").rglob("*.md"))
        for path in docs:
            if path.exists():
                with self.subTest(doc=str(path.relative_to(REPO))):
                    self.assertNotIn(FLAG, path.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
