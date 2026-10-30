"""The release window (#1466): the one 195-minute rule the scheduled train and the guard share. The functions, and the
CLI end to end against a fake gh (a temp executable that prints canned JSON; nothing here reaches GitHub), and the guard
still deciding exactly as before while importing the rule under `python3 -I`."""

import datetime as dt
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import release_train_guard as guard  # noqa: E402
import release_window as window  # noqa: E402

TOOL = ROOT / "tools" / "release_window.py"
GUARD = ROOT / "tools" / "release_train_guard.py"

NOW = dt.datetime(2026, 10, 29, 12, 0, tzinfo=dt.timezone.utc)


def iso(minutes_ago: float) -> str:
    return (NOW - dt.timedelta(minutes=minutes_ago)).strftime("%Y-%m-%dT%H:%M:%SZ")


def release(tag: str, minutes_ago: float, draft: bool = False, prerelease: bool = True) -> dict:
    return {"tagName": tag, "isDraft": draft, "isPrerelease": prerelease, "publishedAt": iso(minutes_ago)}


class Rule(unittest.TestCase):
    def test_the_window_is_195_minutes(self) -> None:
        self.assertEqual(window.WINDOW_MINUTES, 195)

    def test_the_guard_shares_the_rule_it_does_not_copy_it(self) -> None:
        self.assertEqual(guard.WINDOW_MINUTES, 195)
        self.assertIs(guard.release_window, window)

    def test_parse_time_reads_a_z_stamp_as_utc(self) -> None:
        self.assertEqual(window.parse_time("2026-10-29T12:00:00Z"), NOW)

    def test_age_is_in_minutes_rounded_to_one_decimal(self) -> None:
        self.assertEqual(window.age_minutes(NOW, iso(49)), 49.0)
        self.assertEqual(window.age_minutes(NOW, "2026-10-29T11:59:50Z"), 0.2)

    def test_inside_the_window_at_194_and_at_exactly_195(self) -> None:
        self.assertTrue(window.inside_window(NOW, iso(194)))
        self.assertTrue(window.inside_window(NOW, iso(195)))

    def test_outside_the_window_just_past_195(self) -> None:
        self.assertFalse(window.inside_window(NOW, "2026-10-29T08:44:30Z"))
        self.assertFalse(window.inside_window(NOW, iso(196)))


class GuardSharesTheRule(unittest.TestCase):
    """The guard measures its window with this module's functions, and imports them under `python3 -I`."""

    def test_the_guard_decides_with_the_shared_rule_at_the_boundary(self) -> None:
        for minutes, action in ((195.0, "none"), (196.0, "dispatch")):
            with self.subTest(minutes=minutes):
                release = {"tag": "v6.4.0", "published": iso(minutes)}
                self.assertEqual(guard.decide(NOW, [], True, True, release)["action"], action)

    def test_the_guard_runs_under_isolated_mode_with_the_shared_rule_imported(self) -> None:
        # `python3 -I` (the systemd unit's ExecStart) puts neither the script's folder nor the cwd on sys.path.
        with tempfile.TemporaryDirectory() as elsewhere:
            result = subprocess.run(
                [sys.executable, "-I", str(GUARD), "--help"],
                capture_output=True, text=True, cwd=elsewhere,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--dry-run", result.stdout)

    def test_the_guard_main_decides_under_isolated_mode(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [sys.executable, "-I", str(GUARD), "--repo", "", "--dry-run"],
                capture_output=True, text=True, cwd=tmp, env={"PATH": os.environ["PATH"]},
            )
        # No repository: main() stops at its own message, which it can only print once the module imported.
        self.assertEqual(result.returncode, 1)
        self.assertIn("no repository", result.stderr)


class Newest(unittest.TestCase):
    """newest_published: the one function the CLI and the guard both call."""

    def listing(self, *releases: dict) -> str:
        return json.dumps(list(releases))

    def test_it_returns_the_tag_and_the_stamp(self) -> None:
        self.assertEqual(window.newest_published(self.listing(release("v6.4.0", 49))),
                         {"tag": "v6.4.0", "published": iso(49)})

    def test_an_empty_list_refuses_and_names_the_count(self) -> None:
        with self.assertRaises(window.WindowError) as caught:
            window.newest_published("[]")
        self.assertIn("none of the 0 releases fetched is a published vX.Y.Z; refusing to decide", str(caught.exception))

    def test_drafts_and_foreign_tags_only_refuse_and_name_the_count(self) -> None:
        listing = self.listing(release("v6.5.0", 5, draft=True), release("nightly-1", 6), release("v6.4.0-rc1", 7))
        with self.assertRaises(window.WindowError) as caught:
            window.newest_published(listing)
        self.assertIn("none of the 3 releases fetched is a published vX.Y.Z; refusing to decide", str(caught.exception))

    def test_one_published_release_among_drafts_is_found(self) -> None:
        listing = self.listing(*[release(f"v7.{n}.0", 5, draft=True) for n in range(5)], release("v6.4.0", 400))
        self.assertEqual(window.newest_published(listing)["tag"], "v6.4.0")

    def test_a_draft_is_not_counted(self) -> None:
        got = window.newest_published(self.listing(release("v6.5.0", 1, draft=True), release("v6.4.0", 400)))
        self.assertEqual(got["tag"], "v6.4.0")

    def test_the_newest_by_time_wins_whatever_the_order(self) -> None:
        got = window.newest_published(self.listing(release("v6.4.0", 400), release("v6.3.0", 20), release("v6.2.0", 900)))
        self.assertEqual(got["tag"], "v6.3.0")

    def test_unreadable_output_raises(self) -> None:
        for text in ("nope", "{}", "[1]", self.listing({"tagName": "v6.4.0", "isDraft": False, "publishedAt": "x"})):
            with self.subTest(text=text), self.assertRaises(window.WindowError):
                window.newest_published(text)

    def test_a_stamp_with_no_time_zone_raises(self) -> None:
        bad = {"tagName": "v6.4.0", "isDraft": False, "publishedAt": "2026-10-29T11:00:00"}
        with self.assertRaises(window.WindowError):
            window.newest_published(self.listing(bad))


class Cli(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="window-test-"))
        self.gh = self.tmp / "gh"
        self.gh.write_text(
            f"#!{sys.executable} -I\n"
            "import json, os, sys\n"
            "with open(os.environ['FAKE_GH_LOG'], 'a', encoding='utf-8') as log:\n"
            "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "if os.environ.get('FAKE_GH_FAIL'):\n"
            "    sys.stderr.write('fake gh: release list failed\\n')\n"
            "    sys.exit(int(os.environ['FAKE_GH_FAIL']))\n"
            "sys.stdout.write(os.environ['FAKE_RELEASES'])\n",
            encoding="utf-8",
        )
        self.gh.chmod(0o755)
        self.gh_log = self.tmp / "gh.log"
        self.summary = self.tmp / "summary.md"

    def tearDown(self) -> None:
        for path in sorted(self.tmp.rglob("*"), reverse=True):
            path.unlink() if path.is_file() or path.is_symlink() else path.rmdir()
        self.tmp.rmdir()

    def invoke(self, releases: list[dict] | str, gh_fail: int = 0, summary: bool = False,
               gh_flag: bool = True, env_extra: dict | None = None) -> subprocess.CompletedProcess:
        env = {k: v for k, v in os.environ.items() if k not in ("GITHUB_STEP_SUMMARY", "GH_BIN")}
        env["FAKE_GH_LOG"] = str(self.gh_log)
        env["FAKE_RELEASES"] = releases if isinstance(releases, str) else json.dumps(releases)
        if gh_fail:
            env["FAKE_GH_FAIL"] = str(gh_fail)
        if summary:
            self.summary.write_text("earlier line\n", encoding="utf-8")
            env["GITHUB_STEP_SUMMARY"] = str(self.summary)
        env.update(env_extra or {})
        args = [sys.executable, "-I", str(TOOL), "--repo", "owner/name", "--now", NOW.strftime("%Y-%m-%dT%H:%M:%SZ")]
        if gh_flag:
            args += ["--gh", str(self.gh)]
        return subprocess.run(args, capture_output=True, text=True, env=env)

    def decision(self, result: subprocess.CompletedProcess) -> dict:
        self.assertEqual(len(result.stdout.splitlines()), 1, result.stdout)
        return json.loads(result.stdout)

    def test_a_release_49_minutes_old_holds(self) -> None:
        result = self.invoke([release("v6.4.0", 49)])
        self.assertEqual(result.returncode, 10, result.stderr)
        self.assertEqual(self.decision(result), {
            "tool": "release_window",
            "event": "decision",
            "action": "hold",
            "reason": "release_window",
            "last_release": "v6.4.0",
            "last_published": iso(49),
            "age_minutes": 49.0,
            "window_minutes": 195,
        })

    def test_a_release_196_minutes_old_clears(self) -> None:
        result = self.invoke([release("v6.4.0", 196)])
        self.assertEqual(result.returncode, 0, result.stderr)
        decision = self.decision(result)
        self.assertEqual((decision["action"], decision["reason"]), ("clear", "no_recent_release"))
        self.assertEqual((decision["last_release"], decision["age_minutes"]), ("v6.4.0", 196.0))

    def test_exactly_195_minutes_holds_like_the_guard(self) -> None:
        result = self.invoke([release("v6.4.0", 195)])
        self.assertEqual(result.returncode, 10, result.stderr)
        self.assertEqual(self.decision(result)["age_minutes"], 195.0)

    def test_a_draft_newer_than_a_published_release_is_ignored(self) -> None:
        result = self.invoke([release("v6.5.0", 5, draft=True), release("v6.4.0", 300)])
        self.assertEqual(result.returncode, 0, result.stderr)
        decision = self.decision(result)
        self.assertEqual((decision["action"], decision["last_release"]), ("clear", "v6.4.0"))

    def test_list_order_does_not_pick_the_release_the_newest_publication_does(self) -> None:
        result = self.invoke([release("v6.3.0", 600), release("v6.4.0", 49), release("v6.2.0", 900)])
        self.assertEqual(result.returncode, 10, result.stderr)
        self.assertEqual(self.decision(result)["last_release"], "v6.4.0")

    def test_the_newest_by_publication_time_wins_over_the_higher_version(self) -> None:
        result = self.invoke([release("v6.4.0", 500), release("v6.10.0", 30)])
        self.assertEqual(self.decision(result)["last_release"], "v6.10.0")

    def test_a_tag_that_is_not_vX_Y_Z_is_ignored(self) -> None:
        releases = [
            release("nightly", 5), release("v6.4.0-rc1", 6), release("6.4.0", 7),
            release("v6.04.0", 8), release("v6.4", 9), release("v6.4.0", 400),
        ]
        result = self.invoke(releases)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.decision(result)["last_release"], "v6.4.0")

    def assert_refused(self, result: subprocess.CompletedProcess, count: int) -> None:
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn(f"none of the {count} releases fetched is a published vX.Y.Z; refusing to decide", result.stderr)
        self.assertFalse(self.summary.exists() and "decision" in self.summary.read_text(encoding="utf-8"))

    def test_an_empty_release_list_refuses_with_exit_2(self) -> None:
        self.assert_refused(self.invoke([], summary=True), 0)

    def test_only_drafts_and_foreign_tags_refuse_with_exit_2(self) -> None:
        self.assert_refused(self.invoke([release("v6.5.0", 5, draft=True), release("nightly", 6)]), 2)

    def test_twenty_drafts_refuse_and_name_the_count(self) -> None:
        self.assert_refused(self.invoke([release(f"v7.{n}.0", 5, draft=True) for n in range(20)]), 20)

    def test_twenty_nightly_tags_refuse(self) -> None:
        self.assert_refused(self.invoke([release(f"nightly-{n}", 5) for n in range(20)]), 20)

    def test_drafts_and_one_published_release_still_decide(self) -> None:
        drafts = [release(f"v7.{n}.0", 5, draft=True) for n in range(19)]
        held = self.invoke([*drafts, release("v6.4.0", 49)])
        self.assertEqual(held.returncode, 10, held.stderr)
        self.assertEqual(self.decision(held)["last_release"], "v6.4.0")
        cleared = self.invoke([*drafts, release("v6.4.0", 400)])
        self.assertEqual(cleared.returncode, 0, cleared.stderr)
        self.assertEqual(self.decision(cleared)["action"], "clear")

    def test_the_call_is_gh_release_list_with_the_fields_the_rule_reads(self) -> None:
        self.invoke([])
        calls = [json.loads(line) for line in self.gh_log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(calls, [[
            "release", "list", "--repo", "owner/name", "--limit", "20",
            "--json", "tagName,isDraft,isPrerelease,publishedAt",
        ]])

    def test_a_failing_gh_exits_2_and_names_the_call(self) -> None:
        result = self.invoke([], gh_fail=4)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("release list", result.stderr)
        self.assertIn("exit 4", result.stderr)
        self.assertIn("fake gh: release list failed", result.stderr)

    def test_unparsable_json_exits_2_and_names_the_call(self) -> None:
        result = self.invoke("not json")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("release list", result.stderr)

    def test_a_published_release_with_an_unparsable_time_exits_2(self) -> None:
        bad = {"tagName": "v6.4.0", "isDraft": False, "isPrerelease": True, "publishedAt": "yesterday"}
        result = self.invoke([bad])
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("v6.4.0", result.stderr)
        self.assertIn("publishedAt", result.stderr)

    def test_a_published_release_with_no_time_exits_2(self) -> None:
        result = self.invoke([{"tagName": "v6.4.0", "isDraft": False, "isPrerelease": True}])
        self.assertEqual(result.returncode, 2)
        self.assertIn("publishedAt", result.stderr)

    def test_a_draft_with_no_time_is_not_an_error(self) -> None:
        draft = {"tagName": "v6.5.0", "isDraft": True, "isPrerelease": True, "publishedAt": None}
        result = self.invoke([draft, release("v6.4.0", 300)])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_the_decision_is_appended_to_the_step_summary(self) -> None:
        result = self.invoke([release("v6.4.0", 49)], summary=True)
        self.assertEqual(result.returncode, 10, result.stderr)
        lines = self.summary.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "earlier line")
        self.assertEqual(len(lines), 2, lines)
        self.assertEqual(json.loads(lines[1]), json.loads(result.stdout))

    def test_no_step_summary_is_fine_outside_actions(self) -> None:
        result = self.invoke([release("v6.4.0", 49)])
        self.assertEqual(result.returncode, 10, result.stderr)
        self.assertFalse(self.summary.exists())

    def test_GH_BIN_is_used_when_the_gh_flag_is_absent(self) -> None:
        result = self.invoke([release("v6.4.0", 49)], gh_flag=False, env_extra={"GH_BIN": str(self.gh)})
        self.assertEqual(result.returncode, 10, result.stderr)

    def test_the_gh_flag_beats_GH_BIN(self) -> None:
        result = self.invoke([release("v6.4.0", 49)], env_extra={"GH_BIN": str(self.tmp / "missing")})
        self.assertEqual(result.returncode, 10, result.stderr)

    def test_a_gh_that_cannot_run_exits_2(self) -> None:
        result = self.invoke([], env_extra={"GH_BIN": str(self.tmp / "missing")}, gh_flag=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("release list", result.stderr)

    def test_without_now_the_real_clock_is_used(self) -> None:
        real = (dt.datetime.now(dt.timezone.utc) - dt.timedelta(minutes=10)).strftime("%Y-%m-%dT%H:%M:%SZ")
        env = {k: v for k, v in os.environ.items() if k not in ("GITHUB_STEP_SUMMARY", "GH_BIN")}
        env["FAKE_GH_LOG"] = str(self.gh_log)
        env["FAKE_RELEASES"] = json.dumps([{"tagName": "v6.4.0", "isDraft": False, "isPrerelease": True, "publishedAt": real}])
        result = subprocess.run([sys.executable, "-I", str(TOOL), "--repo", "owner/name", "--gh", str(self.gh)],
                                capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 10, result.stderr)


if __name__ == "__main__":
    unittest.main()
