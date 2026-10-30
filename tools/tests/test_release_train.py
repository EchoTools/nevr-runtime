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


def step_block(text: str, name: str) -> str:
    """One step of the decide job, from its `- name:` line to the line before the next step or job."""
    lines = text.splitlines()
    start = lines.index(f"    - name: {name}")
    end = next(
        (i for i in range(start + 1, len(lines)) if lines[i].startswith("    - ") or re.match(r"^  \w+:", lines[i])),
        len(lines),
    )
    return "\n".join(lines[start:end]) + "\n"


def step_script(text: str, name: str) -> str:
    """The `run:` script of the named step, dedented."""
    lines = text.splitlines()
    start = lines.index(f"    - name: {name}")
    run = next(i for i in range(start, len(lines)) if lines[i] == "      run: |")
    body = []
    for line in lines[run + 1 :]:
        if line.strip() and not line.startswith("        "):
            break
        body.append(line)
    return textwrap.dedent("\n".join(body)) + "\n"


def plan_step_script(text: str) -> str:
    """The `run:` script of the step named "Plan the release", dedented."""
    return step_script(text, "Plan the release")


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


class WindowStep(unittest.TestCase):
    """The decide job's "Check the release window" step (#1466): a scheduled run holds when a release was published
    inside the window; a dispatched run (by hand, or the guard's) is never held by it."""

    STEP = "Check the release window"

    def setUp(self) -> None:
        self.text = TRAIN.read_text(encoding="utf-8")
        self.block = step_block(self.text, self.STEP)

    def test_the_step_is_in_decide_before_the_plan(self) -> None:
        names = step_names(self.text, "decide")
        self.assertIn(self.STEP, names)
        self.assertLess(names.index(self.STEP), names.index("Plan the release"), names)

    def test_the_step_runs_only_on_the_schedule(self) -> None:
        self.assertIn("      if: github.event_name == 'schedule'\n", self.block)
        self.assertNotIn("workflow_dispatch", self.block)

    def test_the_step_runs_the_shared_rule_with_the_job_token(self) -> None:
        self.assertIn("      id: window\n", self.block)
        self.assertIn("      GH_TOKEN: ${{ github.token }}\n", self.block)
        self.assertIn('python3 tools/release_window.py --repo "$GITHUB_REPOSITORY"', self.block)

    def test_the_plan_is_skipped_on_hold_and_only_then(self) -> None:
        plan = step_block(self.text, "Plan the release")
        self.assertIn("      if: steps.window.outputs.hold != 'true'\n", plan)

    def test_a_held_run_reports_cut_false_and_the_cut_job_stays_off(self) -> None:
        self.assertIn("cut: ${{ steps.plan.outputs.cut || steps.window.outputs.cut }}", self.text)
        self.assertIn("if: needs.decide.outputs.cut == 'true'", self.text)

    def test_nothing_but_the_window_step_writes_hold(self) -> None:
        self.assertEqual(self.text.count('echo "hold=true"'), 1)
        self.assertIn('echo "hold=true"', self.block)

    def test_the_workflow_header_says_the_run_holds(self) -> None:
        header = self.text.split("\non:\n")[0]
        self.assertIn("195", header)
        self.assertIn("hold", header)

    def run_step(self, tool_exit: int) -> tuple[subprocess.CompletedProcess, list[str]]:
        script = step_script(self.text, self.STEP)
        with tempfile.TemporaryDirectory() as tmp:
            (pathlib.Path(tmp) / "tools").mkdir()
            (pathlib.Path(tmp) / "tools" / "release_window.py").write_text(
                f"import sys\nprint('stub release_window')\nsys.exit({tool_exit})\n", encoding="utf-8")
            output = pathlib.Path(tmp) / "github_output"
            output.write_text("", encoding="utf-8")
            result = subprocess.run(
                ["bash", "-c", script],
                cwd=tmp,
                env={**os.environ, "GITHUB_OUTPUT": str(output), "GITHUB_REPOSITORY": "owner/name"},
                capture_output=True,
                text=True,
            )
            lines = output.read_text(encoding="utf-8").splitlines()
        return result, lines

    def test_exit_10_holds_writes_cut_false_and_the_step_stays_green(self) -> None:
        result, lines = self.run_step(10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("hold=true", lines)
        self.assertIn("cut=false", lines)
        self.assertIn("195", result.stdout)

    def test_exit_0_writes_nothing_and_goes_on(self) -> None:
        result, lines = self.run_step(0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lines, [])

    def test_any_other_exit_fails_the_step_with_that_code(self) -> None:
        for code in (1, 2, 3):
            with self.subTest(code=code):
                result, lines = self.run_step(code)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertEqual(lines, [])
                self.assertIn("release_window.py failed", result.stderr)


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
    """The release body's player line is the author's `release-headline.txt` or one fixed sentence; the commits never
    write it (#1444)."""

    def test_the_fixed_sentence_is_the_fallback(self) -> None:
        self.assertEqual(audience.FALLBACK_PLAYER_LINE, "Fixes and internal changes.")

    def test_the_commits_no_longer_build_a_player_line(self) -> None:
        self.assertFalse(hasattr(audience, "player_line"), "a guess from feat subjects misled players twice")


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
