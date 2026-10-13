"""build.yml's draft path: a workflow_dispatch started on a vX.Y.Z tag ref builds the release's four files,
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

FOUR = ["nevr-runtime-v5.0.0-windows.zip", "SHA256SUMS", "RELEASE-NOTES.md", "nevr-runtime-v5.0.0.zip"]

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

    def test_the_listing_job_alone_holds_the_write_token_and_only_for_a_dispatch_on_a_tag(self):
        drafts = load()["jobs"]["drafts"]
        # GitHub REST, "List releases": "Only users with push access will receive listings for draft releases".
        self.assertEqual(drafts["permissions"], {"contents": "write"})
        self.assertEqual(drafts["if"], "github.event_name == 'workflow_dispatch' && github.ref_type == 'tag'")
        # no checkout, no action, nothing from the repository runs with that token
        self.assertEqual([list(s) for s in drafts["steps"]], [["name", "id", "shell", "env", "run"]])
        self.assertNotIn("uses", " ".join(str(s) for s in drafts["steps"]))
        run = drafts["steps"][0]["run"]
        self.assertIn("set -euo pipefail", run)
        self.assertIn("gh api --paginate", run)
        self.assertEqual(drafts["outputs"], {"releases": "${{ steps.list.outputs.releases }}"})

    def test_the_guard_is_read_only_and_decides_from_the_listing_with_no_token(self):
        guard = load()["jobs"]["guard"]
        self.assertEqual(guard["permissions"], {"contents": "read"})
        self.assertEqual(guard["needs"], "drafts")
        self.assertIn("draft", guard["outputs"])
        find = step_named(guard, "Find the draft release of this tag")
        self.assertEqual(find["if"], "github.event_name == 'workflow_dispatch' && github.ref_type == 'tag'")
        self.assertIn("printf '%s' \"$RELEASES\" | tools/release_draft.sh \"$GITHUB_REF_NAME\"", find["run"])
        self.assertEqual(find["env"], {"RELEASES": "${{ needs.drafts.outputs.releases }}"})
        self.assertNotIn("GH_TOKEN", json.dumps(find))
        # the check step's own token is unchanged (read scope: a published release is visible to it)
        self.assertEqual(load()["jobs"]["guard"]["steps"][0]["uses"].split("@")[0], "actions/checkout")

    def test_only_a_dispatch_on_a_tag_ever_lists_and_a_failed_listing_builds_nothing(self):
        jobs = load()["jobs"]
        guard_if = jobs["guard"]["if"]
        self.assertIn("always()", guard_if)
        self.assertIn("needs.drafts.result != 'failure'", guard_if)
        self.assertIn("needs.drafts.result != 'cancelled'", guard_if)
        # build has an `if` with no status function: success() is implied, so a skipped guard skips it
        self.assertNotIn("always()", jobs["build"]["if"])

    def run_listing(self, pages, rc=0, tag="v5.0.0"):
        """Run the listing step's bash with a fake gh printing one --jq result per page (as gh --paginate does)."""
        run = load()["jobs"]["drafts"]["steps"][0]["run"]
        with tempfile.TemporaryDirectory(prefix="listing-") as tmp:
            gh = Path(tmp) / "gh"
            gh.write_text("#!/usr/bin/env bash\n"
                          f'[ {rc} -eq 0 ] || {{ echo "gh: HTTP 403" >&2; exit {rc}; }}\n'
                          'expr=""; while [ $# -gt 0 ]; do [ "$1" = --jq ] && expr=$2; shift; done\n'
                          'while IFS= read -r page; do printf "%s" "$page" | jq -c "$expr"; done < "$FAKE_PAGES"\n')
            gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
            pages_file = Path(tmp) / "pages"
            pages_file.write_text("".join(json.dumps(p) + "\n" for p in pages))
            out = Path(tmp) / "out"
            out.write_text("")
            env = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
            env.update(PATH=f"{tmp}:{os.environ['PATH']}", FAKE_PAGES=str(pages_file), GITHUB_OUTPUT=str(out),
                       GITHUB_REPOSITORY="EchoTools/nevr-runtime", TAG=tag, GH_TOKEN="x")
            result = subprocess.run(["bash", "-c", run], env=env, capture_output=True, text=True)
            return result.returncode, out.read_text().strip(), result.stderr

    def test_the_listing_keeps_only_the_tags_releases_over_every_page_as_one_json_line(self):
        page1 = [{"tag_name": "v5.0.0", "draft": True, "assets": [{"name": "a.zip"}], "id": 1, "body": "x"},
                 {"tag_name": "v4.9.0", "draft": False, "assets": [{"name": "old.zip"}]}]
        page2 = [{"tag_name": "v5.0.0", "draft": False, "assets": []}]
        rc, out, err = self.run_listing([page1, page2])
        self.assertEqual(rc, 0, err)
        self.assertEqual(json.loads(out.split("=", 1)[1]),
                         [{"draft": True, "assets": ["a.zip"]}, {"draft": False, "assets": []}])
        rc, out, err = self.run_listing([[{"tag_name": "v4.9.0", "draft": False, "assets": []}]])
        self.assertEqual((rc, out), (0, "releases=[]"), err)

    def test_a_failed_listing_fails_the_step_and_writes_no_output(self):
        rc, out, err = self.run_listing([[]], rc=1)
        self.assertNotEqual(rc, 0)
        self.assertEqual(out, "", "no releases=... line: an empty list must never stand for a failed listing")

    def test_a_failed_or_refused_lookup_fails_the_run_red_and_nothing_downstream_runs(self):
        guard = load()["jobs"]["guard"]
        find = step_named(guard, "Find the draft release of this tag")
        self.assertNotIn("continue-on-error", find)
        self.assertNotIn("||", find["run"], "an exit code of the script must not be swallowed")
        self.assertIn("set -euo pipefail", find["run"])
        jobs = load()["jobs"]
        self.assertEqual(jobs["build"]["needs"], "guard", "every later job hangs off build, which hangs off guard")
        self.assertEqual(jobs["build"]["if"], "needs.guard.outputs.built != 'true'")

    def run_lookup(self, script_body, ref="v5.0.0", releases="[]"):
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
            env.update(GITHUB_OUTPUT=str(out), GITHUB_REF_NAME=ref, GITHUB_REPOSITORY="EchoTools/nevr-runtime",
                       RELEASES=releases)
            result = subprocess.run(["bash", "-c", run], cwd=tmp, env=env, capture_output=True, text=True)
            return result.returncode, out.read_text().strip(), result.stderr

    def test_the_draft_output_is_true_only_when_the_script_said_draft(self):
        self.assertEqual(self.run_lookup("echo draft\n")[:2], (0, "draft=true"))
        # the listing reaches the script on stdin
        rc, out, err = self.run_lookup('[ "$(cat)" = \'[{"draft":true,"assets":[]}]\' ] && echo draft || echo none\n',
                                       releases='[{"draft":true,"assets":[]}]')
        self.assertEqual((rc, out), (0, "draft=true"), err)
        self.assertEqual(self.run_lookup("echo none\n")[:2], (0, "draft=false"))

    def test_a_script_failure_fails_the_step_and_writes_no_output(self):
        rc, out, err = self.run_lookup('echo "release_draft: draft v5.0.0 already carries built assets; refusing to overwrite" >&2\nexit 1\n')
        self.assertNotEqual(rc, 0)
        self.assertEqual(out, "", "no draft=... line: nothing downstream may read a decision")

    def test_no_other_job_gains_write_access_for_the_lookup(self):
        jobs = load()["jobs"]
        self.assertEqual(jobs["drafts"]["permissions"]["contents"], "write")
        for name in ("guard", "build", "sign", "seal", "repack"):
            self.assertEqual(jobs[name]["permissions"]["contents"], "read", name)
        self.assertEqual(jobs["publish"]["permissions"]["contents"], "write")

    def test_a_draft_dispatch_builds_the_same_files_as_a_release_event(self):
        jobs = load()["jobs"]
        build = jobs["build"]
        self.assertEqual(build["outputs"]["draft"], "${{ needs.guard.outputs.draft }}")
        # The dist package is built whenever a version is set: a release event, a draft's dispatch and the dry
        # run (the symbol checks and the Windows zip need it), so the draft path cannot lack it.
        dist = step_named(build, "Create distribution packages (unsigned)")
        self.assertEqual(dist["if"], "steps.release.outputs.version != ''")
        self.assertEqual(dist["run"], "cmake --build --preset mingw-release --target dist")
        for name in ("Verify distribution archives", "Upload the unsigned packages for signing"):
            self.assertEqual(step_named(build, name)["if"],
                             "github.event_name == 'release' || needs.guard.outputs.draft == 'true'", name)
        sign = jobs["sign"]
        download = next(s for s in sign["steps"] if s.get("with", {}).get("name") == "unsigned-packages")
        self.assertEqual(download["if"], "github.event_name == 'release' || " + DRAFT)
        self.assertEqual(sign["if"], "github.event_name == 'release' || needs.build.outputs.version != ''")

    def test_repack_and_publish_run_for_a_release_event_as_before_and_for_a_dispatch_only_with_a_draft(self):
        jobs = load()["jobs"]
        gate = ("(github.event_name == 'release' || needs.build.outputs.draft == 'true')")
        repack, publish = jobs["repack"], jobs["publish"]
        self.assertEqual(repack["needs"], ["build", "sign", "seal"])
        self.assertEqual(repack["if"],
            "${{ always() && needs.sign.result == 'success' && needs.seal.result == 'success' && " + gate + " }}")
        self.assertEqual(publish["needs"], ["build", "sign", "seal", "repack"])
        self.assertEqual(publish["if"],
            "${{ always() && needs.sign.result == 'success' && needs.seal.result == 'success' && "
            "needs.repack.result == 'success' && " + gate + " }}")
        # the read-only/write split is kept: the draft path gives repack no write token
        self.assertEqual(repack["permissions"], {"contents": "read"})

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
        for name in ("repack", "publish"):
            gated = load()["jobs"][name]["if"]
            self.assertIn("needs.build.outputs.draft == 'true'", gated, name)
            self.assertNotIn("github.event_name == 'workflow_dispatch' &&", gated.replace(
                "(github.event_name == 'release' || needs.build.outputs.draft == 'true')", ""), name)

    def test_the_guard_and_the_draft_lookup_read_one_list_of_names(self):
        for script in ("release_already_built.sh", "release_draft.sh"):
            text = (REPO / "tools" / script).read_text(encoding="utf-8")
            self.assertIn("release_asset_names.sh", text, script)
            self.assertNotIn("RELEASE-NOTES.md\n", text.split("source ", 1)[1], script + ": a second copy of the list")
            self.assertNotRegex(text.split("source ", 1)[1], r'nevr-runtime-v\$version[-.]', script)
        names = (REPO / "tools" / "release_asset_names.sh").read_text(encoding="utf-8")
        self.assertNotIn("tar.zst\"", names, "the .tar.zst archives are a workflow artifact, not a release asset")
        self.assertEqual(names.count('"nevr-runtime-v$version'), 2)

    # ---- the published guard (tools/release_already_built.sh) meets the draft this path fills -------------
    def built(self, assets):
        tmp = Path(tempfile.mkdtemp(prefix="built-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        gh = tmp / "gh"
        gh.write_text('#!/usr/bin/env bash\n[ "$1 $2" = "release view" ] || exit 99\nprintf "%s\\n" $FAKE_ASSETS\n')
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
        env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}", FAKE_ASSETS=" ".join(assets))
        return subprocess.run([str(BUILT), "v5.0.0", "EchoTools/nevr-runtime"], env=env, capture_output=True, text=True)

    def test_publishing_a_draft_this_path_filled_with_the_four_files_rebuilds_nothing(self):
        result = self.built(FOUR)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "true"), result.stderr)

    def test_a_draft_missing_one_of_the_four_is_built_by_the_published_run(self):
        for missing in FOUR:
            result = self.built([n for n in FOUR if n != missing])
            self.assertEqual((result.returncode, result.stdout.strip()), (0, "false"), missing)


if __name__ == "__main__":
    unittest.main()
