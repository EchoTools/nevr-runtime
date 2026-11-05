"""The draft checks (tools/release_check_draft.sh) and the audience section and player line of the release body
(tools/release_audience_section.py, which tools/package_release.py imports)."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import release_audience_section as audience  # noqa: E402

CHECK = ROOT / "tools" / "release_check_draft.sh"
AUDIENCE_TOOL = ROOT / "tools" / "release_audience_section.py"


class DraftCheck(unittest.TestCase):
    def setUp(self) -> None:
        self.text = CHECK.read_text(encoding="utf-8")

    def test_the_script_fails_on_the_first_error(self) -> None:
        self.assertIn("set -euo pipefail", self.text)
        self.assertNotIn("|| true", self.text)

    def test_it_runs_each_check_the_doc_names(self) -> None:
        for needle in (
            "gh release view",
            "gh attestation verify",
            "sha256sum -c SHA256SUMS",
            "strings -a BugSplat64.dll nevr-runtime.dll",
            "tools/pe_version_info.py check",
        ):
            self.assertIn(needle, self.text)

    def test_the_attestation_loop_is_not_run_on_the_signed_zip(self) -> None:
        self.assertIn('if [ "$signed" -eq 0 ]', self.text)

    def test_the_script_parses(self) -> None:
        result = subprocess.run(["bash", "-n", str(CHECK)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


class AudienceSection(unittest.TestCase):
    def test_a_commit_goes_to_the_audience_of_its_scope(self) -> None:
        out = audience.sections(
            [
                "feat(quest): a new thing",
                "fix(server): the bridge reconnects",
                "feat(plugins): a plugin hook",
            ]
        )
        players = out.split("### For players", 1)[1].split("### For server hosts", 1)[0]
        hosts = out.split("### For server hosts", 1)[1].split("### For plugin developers", 1)[0]
        devs = out.split("### For plugin developers", 1)[1].split("## Not tested", 1)[0]
        self.assertIn("- a new thing", players)
        self.assertIn("- the bridge reconnects", hosts)
        self.assertIn("- a plugin hook", devs)

    def test_a_breaking_change_is_marked(self) -> None:
        out = audience.sections(["feat(runtime)!: drop the old flag"])
        self.assertIn("- BREAKING: drop the old flag", out)

    def test_types_are_case_insensitive(self) -> None:
        out = audience.sections(["Feat(arena): keep the clock running", "FIX: a player fix", "Perf(quest): faster"])
        hosts = out.split("### For server hosts", 1)[1].split("### For plugin developers", 1)[0]
        players = out.split("### For players", 1)[1].split("### For server hosts", 1)[0]
        self.assertIn("- keep the clock running", hosts)
        self.assertIn("- a player fix", players)
        self.assertIn("- faster", players)

    def test_the_scope_is_lowercased_before_lookup(self) -> None:
        out = audience.sections(["fix(Server): the bridge reconnects", "feat(Tools): a worktree recipe"])
        hosts = out.split("### For server hosts", 1)[1].split("### For plugin developers", 1)[0]
        self.assertIn("- the bridge reconnects", hosts)
        self.assertNotIn("a worktree recipe", out)

    def test_a_breaking_change_footer_is_marked(self) -> None:
        for token in ("BREAKING CHANGE", "BREAKING-CHANGE"):
            with self.subTest(token=token):
                out = audience.sections([f"feat(server): a subject\n\nA body.\n\n{token}: the old flag is gone"])
                hosts = out.split("### For server hosts", 1)[1].split("### For plugin developers", 1)[0]
                self.assertIn("- BREAKING: a subject", hosts)

    def test_breaking_change_only_mid_line_is_not_a_footer(self) -> None:
        out = audience.sections(["fix(quest): the subject\n\nThis is not a BREAKING CHANGE: just prose."])
        self.assertIn("- the subject", out)
        self.assertNotIn("BREAKING:", out)

    def test_other_types_are_left_out(self) -> None:
        out = audience.sections(["docs: a note", "ci(release): tag builds skip verify", "chore: tidy"])
        self.assertNotIn("a note", out)
        self.assertNotIn("tag builds skip verify", out)
        self.assertEqual(out.count("Nothing for this audience in this release."), 3)

    def test_tooling_scopes_are_left_out_even_as_features(self) -> None:
        out = audience.sections(["feat(tools): a worktree recipe", "feat(runtime): a real change"])
        self.assertNotIn("a worktree recipe", out)
        self.assertIn("- a real change", out)

    def test_release_tooling_scope_is_left_out_even_as_a_feature_or_a_fix(self) -> None:
        out = audience.sections(["feat(release): a publish recipe", "fix(release): a script", "fix(quest): a real fix"])
        self.assertNotIn("a publish recipe", out)
        self.assertNotIn("a script", out)
        self.assertIn("- a real fix", out)
        self.assertEqual(out.count("Nothing for this audience in this release."), 2)

    def test_the_singular_plugin_scope_is_for_plugin_developers_too(self) -> None:
        out = audience.sections(["feat(plugin): a level field, API v8", "fix(plugin): a loader fix", "fix(quest): a real fix"])
        players = out.split("### For players", 1)[1].split("### For server hosts", 1)[0]
        devs = out.split("### For plugin developers", 1)[1].split("## Not tested", 1)[0]
        self.assertIn("- a level field, API v8", devs)
        self.assertIn("- a loader fix", devs)
        self.assertNotIn("a level field", players)
        self.assertNotIn("a loader fix", players)
        self.assertIn("- a real fix", players)

    def test_an_empty_audience_says_so(self) -> None:
        out = audience.sections(["fix(quest): a thing"])
        self.assertIn("### For plugin developers\n\n- Nothing for this audience in this release.", out)

    def test_it_always_ends_with_what_was_not_tested(self) -> None:
        self.assertTrue(audience.sections([]).rstrip().endswith(audience.NOT_TESTED))

    def test_arena_changes_are_for_server_hosts(self) -> None:
        out = audience.sections(["fix(arena): public Arena on a server starts at 8:00 (refs #982) (#1091)"])
        hosts = out.split("### For server hosts", 1)[1].split("### For plugin developers", 1)[0]
        players = out.split("### For players", 1)[1].split("### For server hosts", 1)[0]
        self.assertIn("public Arena on a server starts at 8:00", hosts)
        self.assertNotIn("public Arena", players)

    def test_a_release_note_line_replaces_the_subject(self) -> None:
        message = (
            "feat(runtime): logs, config.yaml and caches resolve under the nevr root (#1090)\n\n"
            "Longer body.\nRelease-note: logs and caches move to the nevr folder; config.yaml stays\n"
            "Co-Authored-By: Someone <a@example.com>"
        )
        out = audience.sections([message])
        self.assertIn("- logs and caches move to the nevr folder; config.yaml stays", out)
        self.assertNotIn("config.yaml and caches resolve", out)

    def test_a_release_note_keeps_the_breaking_mark_and_the_audience(self) -> None:
        out = audience.sections(["feat(server)!: a subject\n\nRelease-note: what a host sees"])
        hosts = out.split("### For server hosts", 1)[1].split("### For plugin developers", 1)[0]
        self.assertIn("- BREAKING: what a host sees", hosts)

    def test_a_message_without_a_release_note_is_listed_by_its_subject(self) -> None:
        out = audience.sections(["fix(quest): the subject\n\nA body that mentions Release-note: only mid-line."])
        self.assertIn("- the subject", out)

    def test_the_not_tested_text_does_not_say_verify_ran_on_each_pull_request(self) -> None:
        self.assertNotIn("ran on each pull request", audience.NOT_TESTED)
        self.assertIn("quick check", audience.NOT_TESTED)
        self.assertIn("#1008", audience.NOT_TESTED)

    def test_the_log_is_read_with_bodies_and_split_per_commit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            git = ["git", "-C", tmp, "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false"]
            subprocess.run(["git", "init", "-q", tmp], check=True)
            for message in ("chore: base", "feat(arena): one\n\nRelease-note: said by the body", "fix(quest): two"):
                subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", message], check=True)
            subprocess.run(git + ["branch", "base", "HEAD~2"], check=True)
            result = subprocess.run(
                [sys.executable, "-I", str(AUDIENCE_TOOL), "--repo", tmp, "--base", "base", "--rev", "HEAD"],
                capture_output=True, text=True,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("- said by the body", result.stdout)
        self.assertIn("- two", result.stdout)
        self.assertNotIn("- one", result.stdout)


class PlayerLine(unittest.TestCase):
    """The release body's player line is the authors' `release-headlines/` files or one fixed sentence; the commits never
    write it (#1444)."""

    def test_the_fixed_sentence_is_the_fallback(self) -> None:
        self.assertEqual(audience.FALLBACK_PLAYER_LINE, "Fixes and internal changes.")

    def test_the_commits_no_longer_build_a_player_line(self) -> None:
        self.assertFalse(hasattr(audience, "player_line"), "a guess from feat subjects misled players twice")


if __name__ == "__main__":
    unittest.main()
