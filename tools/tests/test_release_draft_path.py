"""build.yml's draft path: a workflow_dispatch started on a vX.Y.Z tag ref builds the release's five files,
attests them and attaches them to that tag's DRAFT release (kept a draft), so a signer can replace the draft's
assets before anything is public; a release event and a dispatch with no draft behave as before.

Structure and the guard script only: nothing is run on GitHub. Cited behaviour (GitHub Docs, "Events that
trigger workflows"): the `release` created/edited/deleted types are not triggered for draft releases;
workflow_dispatch's GITHUB_REF is "Branch or tag that received dispatch". softprops/action-gh-release README: a
reused draft is PUBLISHED after the upload unless `draft: true`, and release keys that are not explicitly set
(prerelease) are retained."""

import json
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "build.yml"
BUILT = REPO / "tools" / "release_already_built.sh"

FIVE = ["nevr-runtime-v5.0.0-windows.zip", "SHA256SUMS", "RELEASE-NOTES.md", "nevr-runtime-v5.0.0.zip",
        "nevr-runtime-v5.0.0-lite.zip"]

DRAFT = "needs.build.outputs.draft == 'true'"


def load():
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


def step_named(job, name):
    return next(step for step in job["steps"] if step.get("name") == name)


class ReleaseDraftPathTest(unittest.TestCase):
    def test_the_trigger_is_still_published_only_and_a_dispatch_still_has_no_inputs(self):
        data = load()
        triggers = data.get("on", data.get(True))
        self.assertEqual(triggers["release"]["types"], ["published"])
        self.assertIn("workflow_dispatch", triggers)
        self.assertFalse((triggers["workflow_dispatch"] or {}).get("inputs"), "no dispatch input is added")

    def test_the_guard_finds_the_draft_of_a_dispatched_tag_with_a_token_that_can_list_drafts(self):
        guard = load()["jobs"]["guard"]
        # GitHub REST, "List releases": "Only users with push access will receive listings for draft releases".
        self.assertEqual(guard["permissions"], {"contents": "write"})
        self.assertIn("draft", guard["outputs"])
        find = step_named(guard, "Find the draft release of this tag")
        self.assertEqual(find["if"], "github.event_name == 'workflow_dispatch' && github.ref_type == 'tag'")
        self.assertIn('tools/release_draft.sh "$GITHUB_REF_NAME" "$GITHUB_REPOSITORY"', find["run"])
        self.assertEqual(find["env"]["GH_TOKEN"], "${{ secrets.GITHUB_TOKEN }}")

    def test_a_failed_or_refused_lookup_fails_the_run_red_and_nothing_downstream_runs(self):
        guard = load()["jobs"]["guard"]
        find = step_named(guard, "Find the draft release of this tag")
        self.assertNotIn("continue-on-error", find)
        self.assertNotIn("||", find["run"], "an exit code of the script must not be swallowed")
        self.assertIn("set -euo pipefail", find["run"])
        jobs = load()["jobs"]
        self.assertEqual(jobs["build"]["needs"], "guard", "every later job hangs off build, which hangs off guard")
        self.assertEqual(jobs["build"]["if"], "needs.guard.outputs.built != 'true'")

    def run_lookup(self, script_body, ref="v5.0.0"):
        """Run the lookup step's bash with a fake tools/release_draft.sh in the working directory."""
        run = step_named(load()["jobs"]["guard"], "Find the draft release of this tag")["run"]
        with tempfile.TemporaryDirectory(prefix="lookup-") as tmp:
            tools = Path(tmp) / "tools"
            tools.mkdir()
            fake = tools / "release_draft.sh"
            fake.write_text("#!/usr/bin/env bash\n" + script_body)
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            out = Path(tmp) / "out"
            out.write_text("")
            env = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
            env.update(GITHUB_OUTPUT=str(out), GITHUB_REF_NAME=ref, GITHUB_REPOSITORY="EchoTools/nevr-runtime")
            result = subprocess.run(["bash", "-c", run], cwd=tmp, env=env, capture_output=True, text=True)
            return result.returncode, out.read_text().strip(), result.stderr

    def test_the_draft_output_is_true_only_when_the_script_said_draft(self):
        self.assertEqual(self.run_lookup("echo draft\n")[:2], (0, "draft=true"))
        self.assertEqual(self.run_lookup("echo none\n")[:2], (0, "draft=false"))

    def test_a_script_failure_fails_the_step_and_writes_no_output(self):
        rc, out, err = self.run_lookup('echo "release_draft: draft v5.0.0 already carries built assets; refusing to overwrite" >&2\nexit 1\n')
        self.assertNotEqual(rc, 0)
        self.assertEqual(out, "", "no draft=... line: nothing downstream may read a decision")

    def test_no_other_job_gains_write_access_for_the_lookup(self):
        jobs = load()["jobs"]
        self.assertEqual(jobs["guard"]["permissions"]["contents"], "write")
        for name in ("build", "sign", "seal"):
            self.assertEqual(jobs[name]["permissions"]["contents"], "read", name)
        self.assertEqual(jobs["publish"]["permissions"]["contents"], "write")

    def test_a_draft_dispatch_builds_the_same_files_as_a_release_event(self):
        jobs = load()["jobs"]
        build = jobs["build"]
        self.assertEqual(build["outputs"]["draft"], "${{ needs.guard.outputs.draft }}")
        for name in ("Create distribution packages (unsigned)", "Verify distribution archives",
                     "Upload the unsigned packages for signing"):
            self.assertEqual(step_named(build, name)["if"],
                             "github.event_name == 'release' || needs.guard.outputs.draft == 'true'", name)
        sign = jobs["sign"]
        download = next(s for s in sign["steps"] if s.get("with", {}).get("name") == "unsigned-packages")
        self.assertEqual(download["if"], "github.event_name == 'release' || " + DRAFT)
        self.assertEqual(sign["if"], "github.event_name == 'release' || needs.build.outputs.version != ''")

    def test_publish_runs_for_a_release_event_as_before_and_for_a_dispatch_only_with_a_draft(self):
        publish = load()["jobs"]["publish"]
        self.assertEqual(publish["needs"], ["build", "sign", "seal"])
        self.assertEqual(publish["if"],
            "${{ always() && needs.sign.result == 'success' && needs.seal.result == 'success' && "
            "(github.event_name == 'release' || needs.build.outputs.draft == 'true') }}")

    def test_both_uploads_keep_a_draft_a_draft_and_never_set_the_prerelease_flag(self):
        steps = load()["jobs"]["publish"]["steps"]
        uploads = [s for s in steps if str(s.get("uses", "")).startswith("softprops/action-gh-release@")]
        self.assertEqual(len(uploads), 2)
        for upload in uploads:
            # true for a dispatch (the action would otherwise PUBLISH the reused draft), false for a release event
            self.assertEqual(upload["with"]["draft"], "${{ github.event_name == 'workflow_dispatch' }}")
            # prerelease stays UNSET: the action then re-sends the release's own flag, so a draft keeps the
            # pre-release flag its human gave it (README: keys not explicitly set are retained)
            self.assertNotIn("prerelease", upload["with"])
            self.assertNotIn("tag_name", upload["with"])

    def test_a_dispatch_without_a_draft_is_the_dry_run_it_was(self):
        # publish is gated on the guard's draft output, which is only 'true' when a draft was found
        publish = load()["jobs"]["publish"]
        self.assertIn("needs.build.outputs.draft == 'true'", publish["if"])
        self.assertNotIn("github.event_name == 'workflow_dispatch' &&", publish["if"].replace(
            "(github.event_name == 'release' || needs.build.outputs.draft == 'true')", ""))

    def test_the_guard_and_the_draft_lookup_read_one_list_of_names(self):
        for script in ("release_already_built.sh", "release_draft.sh"):
            text = (REPO / "tools" / script).read_text(encoding="utf-8")
            self.assertIn("release_asset_names.sh", text, script)
            self.assertNotIn("RELEASE-NOTES.md\n", text.split("source ", 1)[1], script + ": a second copy of the list")
            self.assertNotRegex(text.split("source ", 1)[1], r'nevr-runtime-v\$version[-.]', script)
        names = (REPO / "tools" / "release_asset_names.sh").read_text(encoding="utf-8")
        self.assertNotIn("tar.zst\"", names, "the .tar.zst archives are a workflow artifact, not a release asset")
        self.assertEqual(names.count('"nevr-runtime-v$version'), 3)

    # ---- the published guard (tools/release_already_built.sh) meets the draft this path fills -------------
    def built(self, assets):
        tmp = Path(tempfile.mkdtemp(prefix="built-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        gh = tmp / "gh"
        gh.write_text('#!/usr/bin/env bash\n[ "$1 $2" = "release view" ] || exit 99\nprintf "%s\\n" $FAKE_ASSETS\n')
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
        env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}", FAKE_ASSETS=" ".join(assets))
        return subprocess.run([str(BUILT), "v5.0.0", "EchoTools/nevr-runtime"], env=env, capture_output=True, text=True)

    def test_publishing_a_draft_this_path_filled_with_the_five_files_rebuilds_nothing(self):
        result = self.built(FIVE)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "true"), result.stderr)

    def test_a_draft_missing_one_of_the_five_is_built_by_the_published_run(self):
        for missing in FIVE:
            result = self.built([n for n in FIVE if n != missing])
            self.assertEqual((result.returncode, result.stdout.strip()), (0, "false"), missing)


if __name__ == "__main__":
    unittest.main()
