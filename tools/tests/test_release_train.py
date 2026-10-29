"""The release train (#1010): the workflow's gates, the order of its steps, the draft checks it runs, and the audience
section of the release body. The workflow cannot run here (it publishes), so these read the file and its order."""

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import release_audience_section as audience  # noqa: E402

TRAIN = ROOT / ".github" / "workflows" / "release-train.yml"
CHECK = ROOT / "tools" / "release_check_draft.sh"
AUDIENCE_TOOL = ROOT / "tools" / "release_audience_section.py"


def step_names(text: str, job: str) -> list[str]:
    """The `- name:` lines of one job, in file order."""
    body = re.search(rf"^  {job}:\n(.*?)(?=^  \w+:\n|\Z)", text, re.S | re.M)
    if body is None:
        raise AssertionError(f"release-train.yml has no job {job!r}")
    return re.findall(r"^    - name: (.+)$", body.group(1), re.M)


class TrainWorkflow(unittest.TestCase):
    def setUp(self) -> None:
        self.text = TRAIN.read_text(encoding="utf-8")

    def test_runs_every_three_hours_and_by_hand_only(self) -> None:
        self.assertIn('cron: "17 */3 * * *"', self.text)
        self.assertNotIn('cron: "17 * * * *"', self.text)
        self.assertEqual(self.text.count("- cron:"), 1)
        self.assertIn("workflow_dispatch:", self.text)
        self.assertNotIn("pull_request", self.text)
        self.assertNotIn("\n  push:", self.text)

    def test_nothing_is_cut_unless_the_variable_is_true(self) -> None:
        self.assertEqual(self.text.count("vars.RELEASE_TRAIN == 'true'"), 1)
        self.assertIn("if: vars.RELEASE_TRAIN == 'true'", self.text)
        self.assertIn("if: needs.decide.outputs.cut == 'true'", self.text)

    def test_there_is_no_green_build_gate_on_main(self) -> None:
        # build.yml has no push trigger, so main never has a run: a gate on one never passes.
        self.assertNotIn("--branch main --status success", self.text)
        self.assertNotIn("headSha", self.text)

    def test_the_token_may_dispatch_build_yml(self) -> None:
        permissions = re.search(r"^permissions:\n((?:  .+\n)+)", self.text, re.M)
        self.assertIsNotNone(permissions)
        self.assertIn("  actions: write\n", permissions.group(1))

    def test_the_build_run_is_selected_by_the_tag_and_the_dispatch_time(self) -> None:
        self.assertIn(".headBranch == $tag and .createdAt >= $since", self.text)
        self.assertNotIn("--jq \"[.[] | select(.headBranch", self.text)

    def test_the_signer_run_is_selected_by_the_dispatch_time_and_the_tag(self) -> None:
        self.assertIn("select(.createdAt >= $since)", self.text)
        self.assertIn("select(.displayTitle | contains($tag))", self.text)

    def test_no_run_id_is_the_first_of_a_latest_list(self) -> None:
        self.assertNotIn("--jq '.[0].databaseId'", self.text)
        self.assertNotIn('.[0].databaseId")', self.text)

    def test_the_signer_dispatch_is_real_and_uses_the_named_secret(self) -> None:
        self.assertIn("gh workflow run sign-release.yml --repo thesprockee/release-signer", self.text)
        self.assertIn("-f dry_run=false", self.text)
        self.assertEqual(self.text.count("secrets.RELEASE_SIGNER_TOKEN"), 1)
        self.assertIn("RELEASE_SIGNER_TOKEN is not set", self.text)

    def test_no_step_hides_a_failure(self) -> None:
        self.assertNotIn("|| true", self.text)
        self.assertNotIn("|| echo 0", self.text)
        self.assertNotIn("2>/dev/null", self.text)

    def test_the_steps_run_in_the_order_the_doc_gives(self) -> None:
        names = step_names(self.text, "cut")
        order = [
            "Tag origin's tip",
            "Create the draft",
            "Build the draft's files",
            "Check the draft before signing",
            "Sign the draft",
            "Check the signed draft",
            "Write the release body",
            "Publish the pre-release",
        ]
        self.assertEqual([n for n in names if n in order], order, names)
        self.assertEqual(names[-1], "Publish the pre-release", "publish must be the last step")

    def test_the_tag_push_does_not_need_a_workflow_to_start(self) -> None:
        self.assertIn("gh workflow run build.yml", self.text)


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


def plan_step_script(text: str) -> str:
    """The `run:` script of the step named "Plan the release", dedented."""
    lines = text.splitlines()
    start = lines.index("    - name: Plan the release")
    run = next(i for i in range(start, len(lines)) if lines[i] == "      run: |")
    body = []
    for line in lines[run + 1 :]:
        if line.strip() and not line.startswith("        "):
            break
        body.append(line)
    return textwrap.dedent("\n".join(body)) + "\n"


class PlanStepVersion(unittest.TestCase):
    """The decide job's "Plan the release" step, run for real against a stub of tools/next_release_version.py. The
    tool prints the tag with its leading v (v6.0.0); the step's outputs carry the bare version, since the cut job writes
    "v$VERSION" everywhere."""

    def setUp(self) -> None:
        if shutil.which("jq") is None:
            self.skipTest("jq is not on PATH: the plan step reads the base with jq")
        self.script = plan_step_script(TRAIN.read_text(encoding="utf-8"))

    def run_plan(self, stub_stdout: str, stub_exit: int) -> tuple[subprocess.CompletedProcess, list[str]]:
        with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as scripts:
            git = ["git", "-C", tmp, "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false"]
            subprocess.run(["git", "init", "-q", tmp], check=True)
            subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "chore: base"], check=True)
            subprocess.run(git + ["update-ref", "refs/remotes/origin/main", "HEAD"], check=True)
            (pathlib.Path(tmp) / "tools").mkdir()
            prints = f"print({stub_stdout!r})\n" if stub_stdout else ""
            stub = (
                "import json, sys\n"
                f"{prints}"
                'print(json.dumps({"event": "base", "base": "v5.20.0"}), file=sys.stderr)\n'
                f"sys.exit({stub_exit})\n"
            )
            (pathlib.Path(tmp) / "tools" / "next_release_version.py").write_text(stub, encoding="utf-8")
            script = pathlib.Path(scripts) / "plan.sh"
            script.write_text(self.script, encoding="utf-8")
            output = pathlib.Path(scripts) / "github_output"
            output.write_text("", encoding="utf-8")
            result = subprocess.run(
                ["bash", str(script)],
                cwd=tmp,
                env={**os.environ, "GITHUB_OUTPUT": str(output)},
                capture_output=True,
                text=True,
            )
            lines = output.read_text(encoding="utf-8").splitlines()
        return result, lines

    def test_the_output_carries_the_bare_version_not_the_tag(self) -> None:
        result, lines = self.run_plan("v6.0.0", 0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("version=6.0.0", lines, lines)
        self.assertIn("base=v5.20.0", lines, lines)
        self.assertIn("cut=true", lines, lines)
        self.assertFalse(any(line.startswith("version=v") for line in lines), lines)

    def test_nothing_to_release_writes_cut_false_and_no_version(self) -> None:
        result, lines = self.run_plan("", 1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("cut=false", lines, lines)
        self.assertFalse(any(line.startswith("version=") for line in lines), lines)


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
    """The one line of the release body that says what is new for players: built only from feat commits whose audience
    is players (a fix, perf or other type never contributes); with none it says so."""

    def test_only_player_feats_are_listed_and_fixes_are_left_out(self) -> None:
        # (1) fixes for players do not appear in the line; only the two feats do.
        line = audience.player_line([
            "fix(bridge): hold the game socket across the login-failure LoginSuccess send",
            "fix(login): clear the slot when a socket retires",
            "feat(quest): load libpnsrad behind diagnostics.pnsrad_provider (refs #1220) (#1340)",
            "feat(runtime): keep the clock running",
        ])
        self.assertEqual(line, "Load libpnsrad behind diagnostics.pnsrad_provider; keep the clock running.")

    def test_a_reference_listing_several_numbers_is_stripped(self) -> None:
        line = audience.player_line(["feat(runtime): show the created code on the share page (refs #1278, #906)"])
        self.assertEqual(line, "Show the created code on the share page.")

    def test_a_range_with_only_fixes_says_so(self) -> None:
        # (2) no player feat: the fallback, whatever fixes the range holds.
        self.assertEqual(audience.player_line(["fix(bridge): a fix", "fix(quest): another", "perf(quest): faster"]),
                         "Fixes and internal changes.")

    def test_three_feats_show_two_and_count_the_third(self) -> None:
        # (3)
        self.assertEqual(audience.player_line(["feat(runtime): first", "feat(quest): second", "feat(runtime): third"]),
                         "First; second and 1 more.")

    def test_the_count_is_of_player_feats_only(self) -> None:
        # Three player feats beside three fixes: two shown, one more. Counting every listed change would say "3 more".
        self.assertEqual(audience.player_line([
            "fix(a): a fix", "feat(first): first", "fix(b): another fix", "feat(second): second",
            "feat(third): third", "fix(c): a last fix",
        ]), "First; second and 1 more.")

    def test_a_breaking_feat_counts_as_a_feat(self) -> None:
        # (4) feat! is a feat: it is listed, with its mark; the fix beside it is not.
        self.assertEqual(audience.player_line(["feat(runtime)!: drop the old flag", "fix(quest): a fix"]),
                         "BREAKING: drop the old flag.")

    def test_a_feat_for_server_hosts_does_not_count(self) -> None:
        # (5) feat(server) and feat(ws-bridge) are for server hosts (SCOPE_AUDIENCE): not in the player line.
        self.assertEqual(audience.player_line(["feat(server): a host thing", "feat(ws-bridge): a bridge thing",
                                               "feat(runtime): a player thing"]),
                         "A player thing.")
        self.assertEqual(audience.player_line(["feat(server): only for hosts"]), "Fixes and internal changes.")

    def test_the_first_two_player_feats_are_joined_and_the_rest_counted(self) -> None:
        self.assertEqual(audience.player_line(["feat(runtime): first", "fix(quest): second",
                                               "fix(arena): a host thing", "feat(runtime): third"]),
                         "First; third.")

    def test_one_player_feat_is_the_line(self) -> None:
        self.assertEqual(audience.player_line(["feat(quest): a fix"]), "A fix.")

    def test_two_player_feats_are_joined_with_no_count(self) -> None:
        self.assertEqual(audience.player_line(["feat(runtime): a feature", "feat(quest): a second feature"]),
                         "A feature; a second feature.")

    def test_no_player_feat_says_so(self) -> None:
        self.assertEqual(audience.player_line(["fix(arena): a host thing", "feat(plugin): an api"]),
                         "Fixes and internal changes.")
        self.assertEqual(audience.player_line([]), "Fixes and internal changes.")

    def test_a_release_note_is_the_text_the_player_line_shows(self) -> None:
        self.assertEqual(audience.player_line(["feat(runtime): a subject\n\nRelease-note: what a player sees"]),
                         "What a player sees.")

    def test_the_example_renders_without_its_references_and_capitalised(self) -> None:
        # The feat from the v6.0.0 range, its references stripped, the fixes beside it not listed.
        messages = [
            "fix(bridge): hold the game socket across the login-failure LoginSuccess send",
            "feat(quest): load libpnsrad as the platform provider behind diagnostics.pnsrad_provider (refs #1220) (#1340)",
        ] + [f"feat(quest): change {n}" for n in range(11)]
        self.assertEqual(
            audience.player_line(messages),
            "Load libpnsrad as the platform provider behind diagnostics.pnsrad_provider; change 0 and 10 more.")

    def test_a_text_with_no_reference_is_only_capitalised(self) -> None:
        self.assertEqual(audience.player_line(["feat(runtime): a plain change"]), "A plain change.")

    def test_every_trailing_reference_is_stripped_and_only_trailing_ones(self) -> None:
        # Repeated trailing references go; a parenthesis that is not an issue or PR number stays, and so does an
        # issue number in the middle of the text.
        self.assertEqual(audience.player_line(["feat(quest): keep (refs #1) text (closes #2) (#3)"]),
                         "Keep (refs #1) text.")
        self.assertEqual(audience.player_line(["feat(quest): handle (version 2) and (see notes)"]),
                         "Handle (version 2) and (see notes).")

    def test_a_reference_word_other_than_the_listed_ones_is_kept(self) -> None:
        self.assertEqual(audience.player_line(["feat(quest): a fix (see #12)"]), "A fix (see #12).")

    def test_the_line_ends_with_a_full_stop(self) -> None:
        self.assertEqual(audience.player_line(["feat(quest): a fix (#9)", "feat(quest): another"]),
                         "A fix; another.")


class ReleaseBodyWorkflow(unittest.TestCase):
    """Step 7 of release-train.yml: the body is `package_release.py notes` alone, over the range the tag was cut from."""

    def setUp(self) -> None:
        text = TRAIN.read_text(encoding="utf-8")
        self.step = text.split("    - name: Write the release body\n", 1)[1].split("    # 8. Publish", 1)[0]

    def test_the_body_is_rendered_by_the_notes_stage_over_the_base_and_the_tag(self) -> None:
        self.assertIn('python3 tools/package_release.py notes --assets "$assets" --out "$notes" --repo . '
                      '--base "$BASE" --rev "v$VERSION"', self.step)

    def test_the_audience_section_is_no_longer_appended_to_the_body(self) -> None:
        self.assertNotIn("release_audience_section.py", self.step)
        self.assertNotIn(">> \"$notes\"", self.step)

    def test_the_output_file_is_not_created_before_the_notes_stage_writes_it(self) -> None:
        # The stage refuses an existing output (exit 1, "exists; not overwritten"), so the step must not pre-create it.
        self.assertNotIn("notes=$(mktemp)", self.step)
        self.assertIn("notes=$(mktemp -d)/RELEASE-BODY.md", self.step)


if __name__ == "__main__":
    unittest.main()
