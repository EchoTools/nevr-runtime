"""The release train guard (#1409): the decision rules, the window boundary, the CLI end to end against fake gh and fake
next_release_version.py on PATH, and the shape of .github/workflows/release-train-guard.yml. No test reaches GitHub."""

import datetime as dt
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import release_train_guard as guard  # noqa: E402

GUARD = ROOT / "tools" / "release_train_guard.py"
GUARD_WORKFLOW = ROOT / ".github" / "workflows" / "release-train-guard.yml"
TRAIN = ROOT / ".github" / "workflows" / "release-train.yml"

NOW = dt.datetime(2026, 10, 29, 12, 0, tzinfo=dt.timezone.utc)


def iso(minutes_ago: float) -> str:
    return (NOW - dt.timedelta(minutes=minutes_ago)).strftime("%Y-%m-%dT%H:%M:%SZ")


def real_iso(minutes_ago: float) -> str:
    """The CLI reads the real clock, so its fixtures are relative to it, not to NOW."""
    now = dt.datetime.now(dt.timezone.utc)
    return (now - dt.timedelta(minutes=minutes_ago)).strftime("%Y-%m-%dT%H:%M:%SZ")


def run(run_id: int, minutes_ago: float, status: str = "completed", event: str = "schedule") -> dict:
    return {"id": run_id, "createdAt": iso(minutes_ago), "status": status, "event": event}


class Decide(unittest.TestCase):
    def test_no_runs_at_all_dispatches(self) -> None:
        result = guard.decide(NOW, [], True, True)
        self.assertEqual(result["action"], "dispatch")
        self.assertEqual(result["reason"], "no_run_in_window")
        self.assertIsNone(result["last_run_id"])
        self.assertIsNone(result["last_run_created"])
        self.assertIsNone(result["age_minutes"])

    def test_old_newest_run_dispatches(self) -> None:
        result = guard.decide(NOW, [run(7, 300)], True, True)
        self.assertEqual(result["action"], "dispatch")
        self.assertEqual(result["reason"], "no_run_in_window")
        self.assertEqual(result["last_run_id"], 7)
        self.assertEqual(result["last_run_created"], iso(300))
        self.assertEqual(result["age_minutes"], 300.0)

    def test_switch_off_does_nothing(self) -> None:
        result = guard.decide(NOW, [run(7, 300)], True, False)
        self.assertEqual((result["action"], result["reason"]), ("none", "switch_off"))

    def test_switch_off_beats_an_in_progress_run_and_a_releasable_main(self) -> None:
        result = guard.decide(NOW, [run(8, 1, "in_progress")], True, False)
        self.assertEqual((result["action"], result["reason"]), ("none", "switch_off"))

    def test_nothing_releasable_does_nothing(self) -> None:
        result = guard.decide(NOW, [run(7, 300)], False, True)
        self.assertEqual((result["action"], result["reason"]), ("none", "no_releasable_change"))

    def test_nothing_releasable_beats_an_in_progress_run(self) -> None:
        result = guard.decide(NOW, [run(8, 1, "in_progress")], False, True)
        self.assertEqual((result["action"], result["reason"]), ("none", "no_releasable_change"))

    def test_each_active_status_blocks_the_dispatch(self) -> None:
        for status in ("queued", "in_progress", "waiting", "pending"):
            with self.subTest(status=status):
                result = guard.decide(NOW, [run(9, 900, status)], True, True)
                self.assertEqual((result["action"], result["reason"]), ("none", "train_run_in_progress"))

    def test_an_active_run_older_than_the_window_still_blocks(self) -> None:
        runs = [run(10, 300, "completed"), run(9, 900, "in_progress")]
        result = guard.decide(NOW, runs, True, True)
        self.assertEqual((result["action"], result["reason"]), ("none", "train_run_in_progress"))

    def test_status_in_progress_beats_age(self) -> None:
        # The newest run is outside the window, so only the in-progress rule can stop the dispatch.
        result = guard.decide(NOW, [run(11, 500, "in_progress")], True, True)
        self.assertEqual((result["action"], result["reason"]), ("none", "train_run_in_progress"))

    def test_a_completed_run_is_not_in_progress(self) -> None:
        for status in ("completed", "success", "failure", "cancelled"):
            with self.subTest(status=status):
                result = guard.decide(NOW, [run(12, 900, status)], True, True)
                self.assertEqual(result["action"], "dispatch")

    def test_a_run_created_inside_the_window_does_nothing(self) -> None:
        result = guard.decide(NOW, [run(13, 100)], True, True)
        self.assertEqual((result["action"], result["reason"]), ("none", "recent_run"))
        self.assertEqual(result["age_minutes"], 100.0)
        self.assertEqual(result["last_run_id"], 13)

    def test_194_minutes_is_inside_the_window(self) -> None:
        result = guard.decide(NOW, [run(14, 194)], True, True)
        self.assertEqual((result["action"], result["reason"]), ("none", "recent_run"))
        self.assertEqual(result["age_minutes"], 194.0)

    def test_196_minutes_is_outside_the_window(self) -> None:
        result = guard.decide(NOW, [run(15, 196)], True, True)
        self.assertEqual((result["action"], result["reason"]), ("dispatch", "no_run_in_window"))
        self.assertEqual(result["age_minutes"], 196.0)

    def test_the_newest_run_is_the_first_one_in_the_list(self) -> None:
        # newest first: the first entry is the one the window is measured from, however old the last one is.
        result = guard.decide(NOW, [run(16, 10), run(17, 5000)], True, True)
        self.assertEqual((result["action"], result["reason"]), ("none", "recent_run"))
        self.assertEqual(result["last_run_id"], 16)

    def test_the_result_has_every_field(self) -> None:
        result = guard.decide(NOW, [run(18, 10)], True, True)
        self.assertEqual(
            set(result),
            {"action", "reason", "last_run_id", "last_run_created", "age_minutes"},
        )


def fake_tools(tmp: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    """A bin dir with a fake `gh`, and a fake next_release_version.py beside it. The fake gh answers `run list`,
    `variable list` and `workflow run` from environment variables and records each call in FAKE_GH_LOG."""
    bindir = tmp / "bin"
    bindir.mkdir()
    gh = bindir / "gh"
    gh.write_text(
        f"#!{sys.executable} -I\n"
        "import json, os, sys\n"
        "args = sys.argv[1:]\n"
        "with open(os.environ['FAKE_GH_LOG'], 'a', encoding='utf-8') as log:\n"
        "    log.write(json.dumps(args) + '\\n')\n"
        "if args[:2] == ['run', 'list']:\n"
        "    if os.environ.get('FAKE_RUN_LIST_FAIL'):\n"
        "        sys.stderr.write('fake gh: run list failed\\n')\n"
        "        sys.exit(1)\n"
        "    print(os.environ['FAKE_RUNS'])\n"
        "    sys.exit(0)\n"
        "if args[:2] == ['variable', 'list']:\n"
        "    print(os.environ['FAKE_VARS'])\n"
        "    sys.exit(0)\n"
        "if args[:2] == ['workflow', 'run']:\n"
        "    sys.exit(int(os.environ.get('FAKE_DISPATCH_RC', '0')))\n"
        "sys.stderr.write('fake gh: unexpected ' + repr(args) + '\\n')\n"
        "sys.exit(9)\n",
        encoding="utf-8",
    )
    gh.chmod(0o755)
    version = tmp / "next_release_version.py"
    version.write_text(
        "import os, sys\n"
        "sys.stderr.write('{\"tool\": \"next_release_version\", \"event\": \"next\", \"next\": \"v6.1.0\"}\\n')\n"
        "rc = int(os.environ['FAKE_VERSION_RC'])\n"
        "if rc == 0:\n"
        "    print('v6.1.0')\n"
        "sys.exit(rc)\n",
        encoding="utf-8",
    )
    return bindir, version


class Cli(unittest.TestCase):
    """main() run as the workflow runs it, with the fakes first on PATH and the tool path passed explicitly."""

    def setUp(self) -> None:
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="guard-test-"))
        self.bindir, self.version_tool = fake_tools(self.tmp)
        self.gh_log = self.tmp / "gh.log"
        self.summary = self.tmp / "summary.md"
        self.summary.write_text("", encoding="utf-8")

    def tearDown(self) -> None:
        for path in sorted(self.tmp.rglob("*"), reverse=True):
            path.unlink() if path.is_file() or path.is_symlink() else path.rmdir()
        self.tmp.rmdir()

    def invoke(self, runs: list[dict], variable: str = "true", version_rc: int = 0,
               dispatch_rc: int = 0, summary: bool = True, extra: list[str] | None = None,
               run_list_fail: bool = False) -> subprocess.CompletedProcess:
        env = {k: v for k, v in os.environ.items() if k not in ("GITHUB_STEP_SUMMARY",)}
        env.update({
            "PATH": f"{self.bindir}{os.pathsep}{os.environ['PATH']}",
            "FAKE_GH_LOG": str(self.gh_log),
            "FAKE_RUNS": json.dumps([{"databaseId": r["id"], "createdAt": r["createdAt"], "status": r["status"], "event": r["event"]} for r in runs]),
            "FAKE_VARS": json.dumps([{"name": "RELEASE_TRAIN", "value": variable}]),
            "FAKE_VERSION_RC": str(version_rc),
            "FAKE_DISPATCH_RC": str(dispatch_rc),
            "GH_TOKEN": "fake-token-not-used",
        })
        if run_list_fail:
            env["FAKE_RUN_LIST_FAIL"] = "1"
        if summary:
            env["GITHUB_STEP_SUMMARY"] = str(self.summary)
        args = [sys.executable, str(GUARD), "--repo", "owner/name", "--version-tool", str(self.version_tool)]
        return subprocess.run(args + (extra or []), capture_output=True, text=True, env=env)

    def gh_calls(self) -> list[list[str]]:
        if not self.gh_log.exists():
            return []
        return [json.loads(line) for line in self.gh_log.read_text(encoding="utf-8").splitlines()]

    def test_dispatch_prints_one_decision_line_then_dispatched_and_calls_gh(self) -> None:
        result = self.invoke([run(7, 60 * 24 * 30)])
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 2, result.stdout)
        decision = json.loads(lines[0])
        self.assertEqual(decision["tool"], "release_train_guard")
        self.assertEqual(decision["event"], "decision")
        self.assertEqual(decision["action"], "dispatch")
        self.assertEqual(decision["reason"], "no_run_in_window")
        self.assertIn("ts", decision)
        self.assertEqual(json.loads(lines[1]), {"tool": "release_train_guard", "event": "dispatched"})
        self.assertIn(
            ["workflow", "run", "release-train.yml", "--repo", "owner/name", "--ref", "main"], self.gh_calls()
        )

    def test_the_decision_is_appended_to_the_step_summary(self) -> None:
        result = self.invoke([run(7, 60 * 24 * 30)])
        self.assertEqual(result.returncode, 0, result.stderr)
        summary = self.summary.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(summary), 1, summary)
        self.assertEqual(json.loads(summary[0])["action"], "dispatch")

    def test_no_step_summary_is_fine_outside_actions(self) -> None:
        result = self.invoke([], summary=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_recent_run_prints_one_line_and_does_not_dispatch(self) -> None:
        recent = real_iso(10)
        result = self.invoke([{"id": 21, "createdAt": recent, "status": "completed", "event": "schedule"}])
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1, result.stdout)
        decision = json.loads(lines[0])
        self.assertEqual((decision["action"], decision["reason"]), ("none", "recent_run"))
        self.assertEqual(decision["last_run_id"], 21)
        self.assertFalse(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_switch_off_prints_one_line_and_does_not_dispatch(self) -> None:
        result = self.invoke([], variable="false")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1, result.stdout)
        self.assertEqual(json.loads(lines[0])["reason"], "switch_off")
        self.assertFalse(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_nothing_releasable_prints_one_line_and_does_not_dispatch(self) -> None:
        result = self.invoke([], version_rc=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1, result.stdout)
        decision = json.loads(lines[0])
        self.assertEqual((decision["action"], decision["reason"]), ("none", "no_releasable_change"))
        self.assertFalse(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_an_in_progress_train_prints_one_line_and_does_not_dispatch(self) -> None:
        result = self.invoke([{"id": 22, "createdAt": real_iso(900), "status": "in_progress", "event": "schedule"}])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout.splitlines()[0])["reason"], "train_run_in_progress")
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertFalse(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_dry_run_prints_the_decision_and_does_not_dispatch(self) -> None:
        result = self.invoke([], extra=["--dry-run"])
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1, result.stdout)
        self.assertEqual(json.loads(lines[0])["action"], "dispatch")
        self.assertFalse(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_a_failed_version_tool_fails_loudly(self) -> None:
        result = self.invoke([], version_rc=2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("next_release_version", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_a_failed_run_list_fails_loudly_and_names_the_call(self) -> None:
        result = self.invoke([], run_list_fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gh run list", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_a_failed_dispatch_fails_loudly_and_names_the_call(self) -> None:
        result = self.invoke([], dispatch_rc=4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gh workflow run release-train.yml", result.stderr)
        self.assertNotIn('"event": "dispatched"', result.stdout)


class GuardWorkflow(unittest.TestCase):
    def setUp(self) -> None:
        self.text = GUARD_WORKFLOW.read_text(encoding="utf-8")

    def test_it_runs_hourly_and_by_hand(self) -> None:
        self.assertIn('cron: "47 * * * *"', self.text)
        self.assertEqual(self.text.count("- cron:"), 1)
        self.assertIn("workflow_dispatch:", self.text)
        self.assertIsNone(re.search(r"^\s*pull_request(_target)?:", self.text, re.M))

    def test_the_token_may_dispatch_and_read_contents(self) -> None:
        permissions = re.search(r"^permissions:\n((?:  .+\n)+)", self.text, re.M)
        self.assertIsNotNone(permissions)
        self.assertEqual(permissions.group(1), "  contents: read\n  actions: write\n")

    def test_the_job_runs_the_guard_with_the_token_and_the_repository(self) -> None:
        self.assertIn("timeout-minutes: 10", self.text)
        self.assertIn("python3 tools/release_train_guard.py", self.text)
        self.assertIn("GH_TOKEN: ${{ github.token }}", self.text)
        self.assertIn("GITHUB_REPOSITORY: ${{ github.repository }}", self.text)

    def test_the_checkout_is_the_pinned_one_the_train_uses(self) -> None:
        train = TRAIN.read_text(encoding="utf-8")
        checkout = re.search(r"^    - uses: (actions/checkout@\S+ # v\d+)$", train, re.M)
        self.assertIsNotNone(checkout)
        self.assertIn(f"    - uses: {checkout.group(1)}", self.text)
        self.assertIn("ref: main", self.text)
        self.assertIn("fetch-depth: 0", self.text)

    def test_the_comment_says_why(self) -> None:
        self.assertIn("drops or delays", self.text)

    def test_no_step_hides_a_failure(self) -> None:
        self.assertNotIn("|| true", self.text)
        self.assertNotIn("|| echo", self.text)


if __name__ == "__main__":
    unittest.main()
