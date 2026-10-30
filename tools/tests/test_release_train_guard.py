"""The release train guard (#1409): the decision rules, the window boundary, the CLI end to end against fake gh and fake
next_release_version.py on PATH, the --gh and --log-file options, and the systemd user units that run the guard
(tools/systemd/nevr-train-guard*.{service,timer}). No test reaches GitHub: every gh is a fake."""

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
UNITS = ROOT / "tools" / "systemd"
JUSTFILE = ROOT / "justfile"
SERVICE = "nevr-train-guard.service"
FAILED = "nevr-train-guard-failed.service"
TIMER = "nevr-train-guard.timer"
# Built from parts so that this file does not itself name the removed workflow when the grep test searches the tree.
REMOVED_WORKFLOW = ".github/workflows/release-train" + "-guard.yml"

NOW = dt.datetime(2026, 10, 29, 12, 0, tzinfo=dt.timezone.utc)


def iso(minutes_ago: float) -> str:
    return (NOW - dt.timedelta(minutes=minutes_ago)).strftime("%Y-%m-%dT%H:%M:%SZ")


def real_iso(minutes_ago: float) -> str:
    """The CLI reads the real clock, so its fixtures are relative to it, not to NOW."""
    now = dt.datetime.now(dt.timezone.utc)
    return (now - dt.timedelta(minutes=minutes_ago)).strftime("%Y-%m-%dT%H:%M:%SZ")


def gh_release(tag: str, minutes_ago: float, draft: bool = False) -> dict:
    """A release as `gh release list --json tagName,isDraft,isPrerelease,publishedAt` prints it (the real clock)."""
    return {"tagName": tag, "isDraft": draft, "isPrerelease": True, "publishedAt": None if draft else real_iso(minutes_ago)}


def run(run_id: int, minutes_ago: float, status: str = "completed", event: str = "schedule") -> dict:
    return {"id": run_id, "createdAt": iso(minutes_ago), "status": status, "event": event}


def pub(minutes_ago: float, tag: str = "v6.4.0") -> dict:
    """The newest published release, as release_window.newest_published returns it."""
    return {"tag": tag, "published": iso(minutes_ago)}


class Decide(unittest.TestCase):
    def test_no_release_at_all_dispatches(self) -> None:
        result = guard.decide(NOW, [], True, True, None)
        self.assertEqual(result["action"], "dispatch")
        self.assertEqual(result["reason"], "no_release_in_window")
        self.assertIsNone(result["last_run_id"])
        self.assertIsNone(result["last_run_created"])
        self.assertIsNone(result["last_release"])
        self.assertIsNone(result["last_published"])
        self.assertIsNone(result["age_minutes"])
        self.assertEqual(result["window_minutes"], 195)

    def test_an_old_release_dispatches(self) -> None:
        result = guard.decide(NOW, [run(7, 300)], True, True, pub(300))
        self.assertEqual(result["action"], "dispatch")
        self.assertEqual(result["reason"], "no_release_in_window")
        self.assertEqual(result["last_run_id"], 7)
        self.assertEqual(result["last_run_created"], iso(300))
        self.assertEqual(result["last_release"], "v6.4.0")
        self.assertEqual(result["last_published"], iso(300))
        self.assertEqual(result["age_minutes"], 300.0)

    def test_switch_off_does_nothing(self) -> None:
        result = guard.decide(NOW, [run(7, 300)], True, False, pub(300))
        self.assertEqual((result["action"], result["reason"]), ("none", "switch_off"))

    def test_switch_off_beats_an_in_progress_run_and_a_releasable_main(self) -> None:
        result = guard.decide(NOW, [run(8, 1, "in_progress")], True, False, pub(300))
        self.assertEqual((result["action"], result["reason"]), ("none", "switch_off"))

    def test_nothing_releasable_does_nothing(self) -> None:
        result = guard.decide(NOW, [run(7, 300)], False, True, pub(300))
        self.assertEqual((result["action"], result["reason"]), ("none", "no_releasable_change"))

    def test_nothing_releasable_beats_an_in_progress_run(self) -> None:
        result = guard.decide(NOW, [run(8, 1, "in_progress")], False, True, pub(300))
        self.assertEqual((result["action"], result["reason"]), ("none", "no_releasable_change"))

    def test_each_active_status_blocks_the_dispatch(self) -> None:
        for status in ("queued", "in_progress", "waiting", "pending"):
            with self.subTest(status=status):
                result = guard.decide(NOW, [run(9, 900, status)], True, True, pub(900))
                self.assertEqual((result["action"], result["reason"]), ("none", "train_run_in_progress"))

    def test_an_active_run_older_than_the_window_still_blocks(self) -> None:
        runs = [run(10, 300, "completed"), run(9, 900, "in_progress")]
        result = guard.decide(NOW, runs, True, True, pub(900))
        self.assertEqual((result["action"], result["reason"]), ("none", "train_run_in_progress"))

    def test_an_active_run_beats_a_release_inside_the_window(self) -> None:
        result = guard.decide(NOW, [run(8, 1, "in_progress")], True, True, pub(10))
        self.assertEqual((result["action"], result["reason"]), ("none", "train_run_in_progress"))

    def test_a_completed_run_is_not_in_progress(self) -> None:
        for status in ("completed", "success", "failure", "cancelled"):
            with self.subTest(status=status):
                result = guard.decide(NOW, [run(12, 900, status)], True, True, pub(900))
                self.assertEqual(result["action"], "dispatch")

    def test_a_release_inside_the_window_does_nothing(self) -> None:
        result = guard.decide(NOW, [run(13, 900)], True, True, pub(100))
        self.assertEqual((result["action"], result["reason"]), ("none", "recent_release"))
        self.assertEqual(result["age_minutes"], 100.0)
        self.assertEqual(result["last_release"], "v6.4.0")
        self.assertEqual(result["last_run_id"], 13)

    def test_a_release_49_minutes_old_holds_whatever_the_age_of_the_newest_run(self) -> None:
        result = guard.decide(NOW, [run(13, 900)], True, True, pub(49))
        self.assertEqual((result["action"], result["reason"]), ("none", "recent_release"))
        self.assertEqual(result["age_minutes"], 49.0)

    def test_194_minutes_is_inside_the_window(self) -> None:
        result = guard.decide(NOW, [], True, True, pub(194))
        self.assertEqual((result["action"], result["reason"]), ("none", "recent_release"))
        self.assertEqual(result["age_minutes"], 194.0)

    def test_exactly_195_minutes_is_inside_the_window(self) -> None:
        result = guard.decide(NOW, [], True, True, pub(195))
        self.assertEqual((result["action"], result["reason"]), ("none", "recent_release"))
        self.assertEqual(result["age_minutes"], 195.0)

    def test_195_point_1_minutes_is_outside_the_window(self) -> None:
        release = {"tag": "v6.4.0", "published": "2026-10-29T08:44:54Z"}
        result = guard.decide(NOW, [], True, True, release)
        self.assertEqual((result["action"], result["reason"]), ("dispatch", "no_release_in_window"))
        self.assertEqual(result["age_minutes"], 195.1)

    def test_196_minutes_is_outside_the_window(self) -> None:
        result = guard.decide(NOW, [], True, True, pub(196))
        self.assertEqual((result["action"], result["reason"]), ("dispatch", "no_release_in_window"))
        self.assertEqual(result["age_minutes"], 196.0)

    def test_a_held_run_10_minutes_ago_and_a_release_200_minutes_old_dispatches(self) -> None:
        # A run that held (or found nothing to cut) published nothing: the window is the release's, not the run's.
        result = guard.decide(NOW, [run(14, 10)], True, True, pub(200))
        self.assertEqual((result["action"], result["reason"]), ("dispatch", "no_release_in_window"))
        self.assertEqual(result["last_run_id"], 14)

    def test_a_no_op_run_10_minutes_ago_and_a_release_196_minutes_old_dispatches(self) -> None:
        result = guard.decide(NOW, [run(15, 10)], True, True, pub(196))
        self.assertEqual((result["action"], result["reason"]), ("dispatch", "no_release_in_window"))

    def test_a_run_created_recently_with_no_release_dispatches(self) -> None:
        result = guard.decide(NOW, [run(16, 5)], True, True, None)
        self.assertEqual((result["action"], result["reason"]), ("dispatch", "no_release_in_window"))

    def test_the_result_has_every_field(self) -> None:
        result = guard.decide(NOW, [run(18, 10)], True, True, pub(10))
        self.assertEqual(
            set(result),
            {
                "action", "reason", "last_run_id", "last_run_created",
                "last_release", "last_published", "age_minutes", "window_minutes",
            },
        )


class Defaults(unittest.TestCase):
    def test_the_gh_binary_is_GH_BIN_else_the_publisher_gh(self) -> None:
        self.assertEqual(guard.gh_binary({}), "gh-spritz")
        self.assertEqual(guard.gh_binary({"GH_BIN": "/opt/gh"}), "/opt/gh")

    def test_the_log_is_under_xdg_state_home_else_under_local_state(self) -> None:
        self.assertEqual(
            guard.default_log_path({"XDG_STATE_HOME": "/xdg/state"}),
            pathlib.Path("/xdg/state/nevr-runtime/release-train-guard.jsonl"),
        )
        self.assertEqual(
            guard.default_log_path({}),
            pathlib.Path.home() / ".local" / "state" / "nevr-runtime" / "release-train-guard.jsonl",
        )


def fake_tools(tmp: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    """A bin dir with a fake `gh`, and a fake next_release_version.py beside it. The fake gh answers `run list`,
    `release list`, `variable list` and `workflow run` from environment variables and records each call in FAKE_GH_LOG."""
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
        "if args[:2] == ['release', 'list']:\n"
        "    if os.environ.get('FAKE_RELEASE_LIST_FAIL'):\n"
        "        sys.stderr.write('fake gh: release list failed\\n')\n"
        "        sys.exit(1)\n"
        "    print(os.environ['FAKE_RELEASES'])\n"
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


def decoy_gh(tmp: pathlib.Path) -> pathlib.Path:
    """A `gh` that fails loudly if anything runs it: placed first on PATH, it proves the guard used the --gh binary."""
    decoy = tmp / "decoy"
    decoy.mkdir()
    gh = decoy / "gh"
    gh.write_text(f"#!{sys.executable} -I\nimport sys\nsys.stderr.write('decoy gh used\\n')\nsys.exit(77)\n",
                  encoding="utf-8")
    gh.chmod(0o755)
    return decoy


class Cli(unittest.TestCase):
    """main() run as the workflow runs it, with the fakes first on PATH and the tool path passed explicitly."""

    def setUp(self) -> None:
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="guard-test-"))
        self.bindir, self.version_tool = fake_tools(self.tmp)
        self.gh_log = self.tmp / "gh.log"
        self.summary = self.tmp / "summary.md"
        self.summary.write_text("", encoding="utf-8")
        self.log_file = self.tmp / "state" / "release-train-guard.jsonl"

    def tearDown(self) -> None:
        for path in sorted(self.tmp.rglob("*"), reverse=True):
            path.unlink() if path.is_file() or path.is_symlink() else path.rmdir()
        self.tmp.rmdir()

    def invoke(self, runs: list[dict], variable: str = "true", version_rc: int = 0,
               dispatch_rc: int = 0, summary: bool = True, extra: list[str] | None = None,
               run_list_fail: bool = False, gh_flag: bool = True, env_extra: dict | None = None,
               path_front: pathlib.Path | None = None, log_file: pathlib.Path | None = None,
               releases: list[dict] | str | None = None, release_list_fail: bool = False,
               ) -> subprocess.CompletedProcess:
        # GH_BIN is removed from the inherited environment: a developer's value must not reach a test.
        env = {k: v for k, v in os.environ.items() if k not in ("GITHUB_STEP_SUMMARY", "GH_BIN")}
        front = [str(path_front)] if path_front else []
        env.update({
            "PATH": os.pathsep.join([*front, str(self.bindir), os.environ["PATH"]]),
            "FAKE_GH_LOG": str(self.gh_log),
            "FAKE_RUNS": json.dumps([{"databaseId": r["id"], "createdAt": r["createdAt"], "status": r["status"], "event": r["event"]} for r in runs]),
            "FAKE_RELEASES": releases if isinstance(releases, str) else json.dumps(releases or []),
            "FAKE_VARS": json.dumps([{"name": "RELEASE_TRAIN", "value": variable}]),
            "FAKE_VERSION_RC": str(version_rc),
            "FAKE_DISPATCH_RC": str(dispatch_rc),
            "GH_TOKEN": "fake-token-not-used",
        })
        env.update(env_extra or {})
        if run_list_fail:
            env["FAKE_RUN_LIST_FAIL"] = "1"
        if release_list_fail:
            env["FAKE_RELEASE_LIST_FAIL"] = "1"
        if summary:
            env["GITHUB_STEP_SUMMARY"] = str(self.summary)
        args = [sys.executable, str(GUARD), "--repo", "owner/name", "--version-tool", str(self.version_tool),
                "--log-file", str(log_file or self.log_file)]
        if gh_flag:
            args += ["--gh", str(self.bindir / "gh")]
        return subprocess.run(args + (extra or []), capture_output=True, text=True, env=env)

    def gh_calls(self) -> list[list[str]]:
        if not self.gh_log.exists():
            return []
        return [json.loads(line) for line in self.gh_log.read_text(encoding="utf-8").splitlines()]

    def logged(self) -> list[dict]:
        return [json.loads(line) for line in self.log_file.read_text(encoding="utf-8").splitlines()]

    def test_dispatch_prints_one_decision_line_then_dispatched_and_calls_gh(self) -> None:
        result = self.invoke([run(7, 60 * 24 * 30)])
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 2, result.stdout)
        decision = json.loads(lines[0])
        self.assertEqual(decision["tool"], "release_train_guard")
        self.assertEqual(decision["event"], "decision")
        self.assertEqual(decision["action"], "dispatch")
        self.assertEqual(decision["reason"], "no_release_in_window")
        self.assertIsNone(decision["last_release"])
        self.assertEqual(decision["window_minutes"], 195)
        self.assertIn("ts", decision)
        self.assertEqual(json.loads(lines[1]), {"tool": "release_train_guard", "event": "dispatched"})
        self.assertIn(
            ["workflow", "run", "release-train.yml", "--repo", "owner/name", "--ref", "main"], self.gh_calls()
        )

    def test_the_log_file_receives_the_decision_line_and_the_dispatched_line(self) -> None:
        result = self.invoke([run(7, 60 * 24 * 30)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.logged(), [json.loads(line) for line in result.stdout.splitlines()])
        self.assertEqual([record["event"] for record in self.logged()], ["decision", "dispatched"])

    def test_the_log_file_and_its_parent_are_created(self) -> None:
        self.assertFalse(self.log_file.parent.exists())
        result = self.invoke([], releases=[gh_release("v6.4.0", 10)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([record["reason"] for record in self.logged()], ["recent_release"])

    def test_a_failed_run_list_is_recorded_in_the_log_as_a_fail_line(self) -> None:
        result = self.invoke([], run_list_fail=True)
        self.assertNotEqual(result.returncode, 0)
        fails = [record for record in self.logged() if record["event"] == "fail"]
        self.assertEqual(len(fails), 1, self.logged())
        self.assertIn("gh run list", fails[0]["reason"])

    def test_an_unwritable_log_fails_loudly_and_names_the_path_before_any_gh_call(self) -> None:
        blocker = self.tmp / "not-a-dir"
        blocker.write_text("a file, so the log cannot have a parent directory here\n", encoding="utf-8")
        unwritable = blocker / "release-train-guard.jsonl"
        result = self.invoke([], log_file=unwritable)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(unwritable), result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(self.gh_calls(), [], "a run that cannot record its decision must not call gh")

    def test_the_gh_flag_is_the_gh_every_call_uses(self) -> None:
        result = self.invoke([run(7, 60 * 24 * 30)], path_front=decoy_gh(self.tmp))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("decoy gh used", result.stderr)
        self.assertIn(["workflow", "run", "release-train.yml", "--repo", "owner/name", "--ref", "main"],
                      self.gh_calls())

    def test_GH_BIN_is_used_when_the_gh_flag_is_absent(self) -> None:
        result = self.invoke([run(7, 60 * 24 * 30)], gh_flag=False, path_front=decoy_gh(self.tmp),
                             env_extra={"GH_BIN": str(self.bindir / "gh")})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(["workflow", "run", "release-train.yml", "--repo", "owner/name", "--ref", "main"],
                      self.gh_calls())

    def test_the_decision_is_appended_to_the_step_summary(self) -> None:
        result = self.invoke([run(7, 60 * 24 * 30)])
        self.assertEqual(result.returncode, 0, result.stderr)
        summary = self.summary.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(summary), 1, summary)
        self.assertEqual(json.loads(summary[0])["action"], "dispatch")

    def test_no_step_summary_is_fine_outside_actions(self) -> None:
        result = self.invoke([], summary=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_recent_release_prints_one_line_and_does_not_dispatch(self) -> None:
        old_run = {"id": 21, "createdAt": real_iso(900), "status": "completed", "event": "schedule"}
        result = self.invoke([old_run], releases=[gh_release("v6.4.0", 49)])
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 1, result.stdout)
        decision = json.loads(lines[0])
        self.assertEqual((decision["action"], decision["reason"]), ("none", "recent_release"))
        self.assertEqual(decision["last_run_id"], 21)
        self.assertEqual(decision["last_release"], "v6.4.0")
        self.assertFalse(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_a_recent_run_with_an_old_release_still_dispatches(self) -> None:
        # The held or no-op run 10 minutes ago published nothing; the release 200 minutes ago is out of the window.
        recent_run = {"id": 22, "createdAt": real_iso(10), "status": "completed", "event": "schedule"}
        result = self.invoke([recent_run], releases=[gh_release("v6.4.0", 200)])
        self.assertEqual(result.returncode, 0, result.stderr)
        decision = json.loads(result.stdout.splitlines()[0])
        self.assertEqual((decision["action"], decision["reason"]), ("dispatch", "no_release_in_window"))
        self.assertTrue(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_a_draft_newer_than_the_published_release_does_not_hold(self) -> None:
        result = self.invoke([], releases=[gh_release("v6.5.0", 5, draft=True), gh_release("v6.4.0", 400)])
        self.assertEqual(result.returncode, 0, result.stderr)
        decision = json.loads(result.stdout.splitlines()[0])
        self.assertEqual((decision["action"], decision["last_release"]), ("dispatch", "v6.4.0"))

    def test_the_window_comes_from_the_shared_release_list_call(self) -> None:
        self.invoke([], releases=[gh_release("v6.4.0", 49)])
        self.assertIn(["release", "list", "--repo", "owner/name", "--limit", "20",
                       "--json", "tagName,isDraft,isPrerelease,publishedAt"], self.gh_calls())

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

    def test_a_failed_release_list_fails_loudly_names_the_call_and_is_logged(self) -> None:
        result = self.invoke([], release_list_fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gh release list", result.stderr)
        self.assertEqual(result.stdout, "")
        fails = [record for record in self.logged() if record["event"] == "fail"]
        self.assertEqual(len(fails), 1, self.logged())
        self.assertFalse(any(call[:2] == ["workflow", "run"] for call in self.gh_calls()))

    def test_an_unreadable_release_list_fails_loudly(self) -> None:
        result = self.invoke([], releases="not json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release list", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_a_published_release_with_no_time_fails_loudly(self) -> None:
        result = self.invoke([], releases=[{"tagName": "v6.4.0", "isDraft": False, "isPrerelease": True}])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("publishedAt", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_a_failed_dispatch_fails_loudly_and_names_the_call(self) -> None:
        result = self.invoke([], dispatch_rc=4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gh workflow run release-train.yml", result.stderr)
        self.assertNotIn('"event": "dispatched"', result.stdout)


class Units(unittest.TestCase):
    """The three user units that run the guard. They mirror tools/systemd/nevr-quest-release*: the same dedicated
    checkout, the same PATH, and a failure couriers the release lead, never a bonk."""

    def unit(self, name: str) -> str:
        return (UNITS / name).read_text(encoding="utf-8")

    def test_the_three_units_exist(self) -> None:
        for name in (SERVICE, FAILED, TIMER):
            with self.subTest(unit=name):
                self.assertTrue((UNITS / name).is_file())

    def test_the_timer_runs_hourly_at_seven_past_and_catches_up(self) -> None:
        text = self.unit(TIMER)
        self.assertRegex(text, r"(?m)^OnCalendar=\*:07:00$")
        self.assertRegex(text, r"(?m)^Persistent=true$")
        self.assertRegex(text, r"(?m)^AccuracySec=1min$")
        self.assertRegex(text, rf"(?m)^Unit={re.escape(SERVICE)}$")
        self.assertRegex(text, r"(?m)^WantedBy=timers\.target$")

    def test_the_service_is_a_oneshot_with_a_failure_hook_and_a_timeout(self) -> None:
        text = self.unit(SERVICE)
        self.assertRegex(text, r"(?m)^Type=oneshot$")
        self.assertRegex(text, rf"(?m)^OnFailure={re.escape(FAILED)}$")
        self.assertRegex(text, r"(?m)^TimeoutStartSec=10min$")
        self.assertRegex(text, r"(?m)^Environment=PATH=%h/\.local/bin:/usr/local/bin:/usr/bin$")
        self.assertNotIn("GNUPGHOME", text)

    def test_the_service_fetches_then_switches_the_dedicated_checkout_to_origin_main(self) -> None:
        text = self.unit(SERVICE)
        checkout = "%h/src/nevr-runtime-work/release-timer/checkout"
        self.assertRegex(text, rf"(?m)^WorkingDirectory={re.escape(checkout)}$")
        self.assertRegex(text, rf"(?m)^ExecStartPre=/usr/bin/git -C {re.escape(checkout)} fetch --quiet --tags origin main$")
        self.assertRegex(text, rf"(?m)^ExecStartPre=/usr/bin/git -C {re.escape(checkout)} switch --quiet --detach origin/main$")

    def test_the_service_runs_the_guard_from_the_checkout_against_the_repository(self) -> None:
        text = self.unit(SERVICE)
        self.assertRegex(
            text,
            r"(?m)^ExecStart=/usr/bin/python3 -I %h/src/nevr-runtime-work/release-timer/checkout/tools/"
            r"release_train_guard\.py --repo EchoTools/nevr-runtime$",
        )

    def test_the_failed_unit_couriers_spritz_the_log_path_and_never_bonks(self) -> None:
        text = self.unit(FAILED)
        self.assertRegex(
            text,
            r"(?m)^ExecStart=%h/\.local/bin/courier send --to spritz --from release-train-guard "
            r"--msg \"release train guard failed: see %h/\.local/state/nevr-runtime/release-train-guard\.jsonl\"$",
        )
        self.assertNotRegex(text.lower(), r"bonk")

    def test_the_install_recipe_links_the_three_units_and_enables_the_timer(self) -> None:
        justfile = JUSTFILE.read_text(encoding="utf-8")
        match = re.search(r"(?m)^install-train-guard-timer:\n((?:[ \t]+.*\n|\n)*)", justfile)
        self.assertIsNotNone(match, "no install-train-guard-timer recipe in the justfile")
        recipe = match.group(1)
        for name in (SERVICE, FAILED, TIMER):
            with self.subTest(unit=name):
                self.assertIn(name, recipe)
        self.assertIn("systemctl --user link", recipe)
        self.assertIn("systemctl --user daemon-reload", recipe)
        self.assertIn(f"systemctl --user enable --now {TIMER}", recipe)
        self.assertIn("tools/systemd/", recipe)

    def test_the_workflow_file_the_guard_replaces_is_gone(self) -> None:
        self.assertFalse((ROOT / REMOVED_WORKFLOW).exists())

    def test_nothing_tracked_still_names_the_removed_workflow(self) -> None:
        name = REMOVED_WORKFLOW.rsplit("/", 1)[1]
        found = subprocess.run(["git", "-C", str(ROOT), "grep", "-n", "-F", name],
                               capture_output=True, text=True)
        self.assertEqual(found.returncode, 1, f"tracked files still name {name}:\n{found.stdout}{found.stderr}")


if __name__ == "__main__":
    unittest.main()
