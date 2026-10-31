"""The release workflow (build.yml): structure and the small scripts it runs; nothing is published here.

A release is a plain semver tag plus GitHub's pre-release flag. Nothing in this workflow signs: the files on
the draft are signed afterwards by the release signer (RELEASING.md), so no job here claims or skips it. These
tests pin that the packaging path is one path for every release event, that nothing in it reads the
pre-release flag (promotion changes no byte), that a release which already carries its assets is not rebuilt,
and that no job pretends to sign."""

import os
import re
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "build.yml"
GUARD = REPO / "tools" / "release_already_built.sh"

TWO = ["nevr-runtime-v5.0.0-windows.zip", "nevr-runtime-v5.0.0-quest-libovrplatformloader.so"]  # the release's two assets


def load():
    data = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    # PyYAML reads the bare key `on` as True.
    return data, data.get("on", data.get(True))


def steps_text(job: dict) -> str:
    return yaml.safe_dump(job.get("steps", []), width=10**9)  # no line folding inside a phrase


def lines(value: str) -> list:
    return [line.strip() for line in value.splitlines() if line.strip()]


FAKE_GH = """#!/usr/bin/env bash
# Fake gh: `release view` prints $FAKE_ASSETS (or fails with $FAKE_VIEW_RC) and logs the call.
set -u
echo "$*" >> "$FAKE_GH_LOG"
if [ "$1 $2" = "release view" ]; then
  [ "${FAKE_VIEW_RC:-0}" -eq 0 ] || { echo "gh: HTTP 404: Not Found" >&2; exit "${FAKE_VIEW_RC}"; }
  printf '%s' "$FAKE_ASSETS"
  exit 0
fi
echo "unexpected gh call: $*" >&2
exit 99
"""


class WorkflowStructureTest(unittest.TestCase):
    def test_a_release_fires_on_published_only_and_a_dispatch_has_no_inputs(self):
        _, triggers = load()
        self.assertEqual(triggers["release"]["types"], ["published"],
                         "created misses a published draft; released/edited would rebuild on promotion")
        self.assertIsNone(triggers["workflow_dispatch"])

    def test_runs_of_one_event_on_one_ref_share_a_group_and_other_events_or_refs_do_not(self):
        """The group is the event and the ref, not the run id: a run id is unique, so every run had its own
        group and two runs of one event on one tag (a release published again, a second dispatch) could
        upload at once. Runs in one group queue; a new run cancels the one pending in its group, never the
        one in progress, so a release run only ever replaces a pending release run of the same tag and a
        dry-run dispatch can never take its place. A release run and a dispatch run on the SAME tag are in
        different groups and run concurrently: this key does not serialize them."""
        concurrency = load()[0]["concurrency"]
        self.assertIs(concurrency["cancel-in-progress"], False)
        template = concurrency["group"]
        self.assertNotIn("run_id", template)

        def group(event, ref):
            rendered = template.replace("${{ github.event_name }}", event).replace("${{ github.ref }}", ref)
            self.assertNotIn("${{", rendered, f"{template}: a context this test does not model")
            return rendered

        tag, other = "refs/tags/v5.0.1", "refs/tags/v5.0.2"
        self.assertNotEqual(group("release", tag), group("release", other), "another tag does not queue")
        self.assertNotEqual(group("workflow_dispatch", tag), group("workflow_dispatch", other))
        self.assertNotEqual(group("workflow_dispatch", "refs/heads/a"), group("workflow_dispatch", "refs/heads/b"))
        self.assertNotEqual(group("release", tag), group("workflow_dispatch", tag),
                            "a dry run must not be able to cancel the pending release run")
        # What the key pins: the same event on the same ref renders one group (a new run replaces the
        # pending one, an earlier one finishes first).
        self.assertIn("${{ github.event_name }}", template)
        self.assertIn("${{ github.ref }}", template)

    def test_no_comment_or_step_uses_the_retired_model_words(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(re.findall(r"(?i)release candidate|\bcandidate\b|\brc\b|-rc\b|\brc[-_.]|rc_|NEVR_RC", text), [])

    def test_no_comment_or_step_claims_the_files_are_signed(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        for claim in ("Artifact Signing", "signed by the"):
            self.assertNotIn(claim, text)

    def test_nothing_runs_on_windows_logs_into_a_cloud_or_uses_an_environment(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        data, _ = load()
        for name, job in data["jobs"].items():
            self.assertEqual(job["runs-on"], "ubuntu-latest", name)
            self.assertNotIn("environment", job, name)
        for forbidden in ("windows-latest", "azure/", "codesign", "Get-AuthenticodeSignature", "signtool"):
            self.assertNotIn(forbidden, text)

    def test_there_is_no_sign_job_and_nothing_passes_files_through_as_unsigned(self):
        data, _ = load()
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertNotIn("sign", data["jobs"])
        self.assertNotIn("signing stub", text)
        for name, job in data["jobs"].items():
            needs = job.get("needs", [])
            self.assertNotIn("sign", [needs] if isinstance(needs, str) else needs, name)
        for gone in ("needs.sign", "UNSIGNED passthrough", "packages-unsigned", "policy signer not built yet",
                     "PLACEHOLDER: the signer dispatch goes here"):
            self.assertNotIn(gone, text)
        self.assertNotRegex(text, r"name: signed-")

    def test_the_job_graph_is_drafts_guard_build_quest_seal_repack_publish(self):
        jobs = load()[0]["jobs"]
        # `drafts` lists the tag's releases for a dispatch on a tag (write token, no checkout); guard reads it
        self.assertEqual(list(jobs), ["drafts", "guard", "installer", "build", "quest", "seal", "repack", "publish"])
        self.assertEqual(jobs["guard"]["needs"], "drafts")
        # the core installer is its own job off the guard, built for a release event or a dispatch on a tag, and build
        # waits for its artifact in a step rather than through `needs` (#1770)
        self.assertEqual(jobs["installer"]["needs"], "guard")
        self.assertIn("!cancelled()", jobs["installer"]["if"])
        self.assertIn("needs.guard.outputs.built != 'true'", jobs["installer"]["if"])
        self.assertIn("github.event_name == 'release' || github.ref_type == 'tag'", jobs["installer"]["if"])
        self.assertEqual(jobs["build"]["needs"], "guard")
        self.assertIn("needs.guard.outputs.built != 'true'", jobs["build"]["if"])
        self.assertEqual(jobs["seal"]["needs"], "build")
        # repack and publish also need build: they read build's `draft` output (a dispatch on a draft's tag, see
        # test_release_draft_path.py); `needs` outputs are only visible from a direct dependency.
        self.assertEqual(sorted(jobs["repack"]["needs"]), ["build", "seal"])
        self.assertEqual(sorted(jobs["publish"]["needs"]), ["build", "quest", "repack", "seal"])
        # the Quest library is its own job off the guard, built for a release event or a dispatch on a tag, and
        # never when the release already carries its assets
        self.assertEqual(jobs["quest"]["needs"], "guard")
        self.assertIn("needs.guard.outputs.built != 'true'", jobs["quest"]["if"])
        self.assertIn("github.event_name == 'release' || github.ref_type == 'tag'", jobs["quest"]["if"])
        self.assertEqual(jobs["quest"]["permissions"], {"contents": "read"})
        for name in ("seal", "repack", "publish"):
            self.assertNotIn("rc", name)

    def test_the_quest_asset_name_has_one_source_the_guard_also_reads(self):
        # A name written inline in build.yml can drift from tools/release_asset_names.sh, and then the guard
        # never sees both names and a published, signed release is rebuilt over. The quest job must take
        # its output name from release_quest_asset_name, and that name must be one the list yields.
        jobs = load()[0]["jobs"]
        naming = [s for s in jobs["quest"]["steps"] if s.get("id") == "asset"]
        self.assertEqual(len(naming), 1)
        run = naming[0]["run"]
        self.assertIn("source tools/release_asset_names.sh", run)
        self.assertIn('$(release_quest_asset_name "$version")', run)
        self.assertNotIn("-quest-", run, "the release name must not be written inline")
        names = REPO / "tools" / "release_asset_names.sh"
        quest = subprocess.run(["bash", "-c", f'source "{names}"; release_quest_asset_name 9.8.7'],
                               capture_output=True, text=True, check=True).stdout.strip()
        listed = subprocess.run(["bash", "-c", f'source "{names}"; release_asset_names 9.8.7'],
                                capture_output=True, text=True, check=True).stdout.split()
        self.assertIn(quest, listed)

    def test_no_job_below_a_skippable_ancestor_relies_on_the_implicit_success(self):
        # GitHub prepends success() to an `if:` with no status function, and success() is false when ANY
        # ancestor job (not only a direct `needs`) was skipped. `drafts` is skipped on a release event, so a
        # job under it with a bare condition is skipped there and the run is green and builds nothing
        # (v5.1.0, run 38409770193). A dispatch on a tag runs `drafts`, so the dry run cannot see this: it
        # is only visible as a rule over the graph.
        jobs = load()[0]["jobs"]

        def needs(name):
            n = jobs[name].get("needs", [])
            return [n] if isinstance(n, str) else list(n)

        def ancestors(name):
            seen = set()
            todo = needs(name)
            while todo:
                j = todo.pop()
                if j not in seen:
                    seen.add(j)
                    todo.extend(needs(j))
            return seen

        # `!cancelled()` or `always()` only: a bare success() IS the implicit condition, and cancelled() or
        # failure() alone mean something else.
        status = re.compile(r"!cancelled\(\)|\balways\(\)")
        skippable = {n for n, j in jobs.items() if "if" in j}
        self.assertIn("drafts", skippable)
        for name, job in jobs.items():
            if ancestors(name) & skippable:
                self.assertRegex(str(job.get("if", "")), status, f"{name} has a skippable ancestor and no status function")
        # and each names the result of what it needs, so a failed or skipped need still stops it
        self.assertIn("needs.guard.result == 'success'", jobs["build"]["if"])
        self.assertIn("needs.build.result == 'success'", jobs["seal"]["if"])
        self.assertIn("needs.seal.result == 'success'", jobs["repack"]["if"])
        for name in ("build", "seal", "repack", "publish"):
            self.assertIn("!cancelled()", jobs[name]["if"])
            self.assertNotRegex(jobs[name]["if"], r"(?<![!\w])success\(\)", name)

    def test_nothing_is_published_without_a_release_event_or_a_draft_of_the_dispatched_tag(self):
        data, _ = load()
        for name, job in data["jobs"].items():
            uses_release_action = "softprops/action-gh-release" in steps_text(job)
            self.assertEqual(uses_release_action, name == "publish", name)
        publish = data["jobs"]["publish"]
        self.assertIn("github.event_name == 'release'", publish["if"])
        self.assertIn("needs.seal.result == 'success'", publish["if"])
        self.assertIn("needs.repack.result == 'success'", publish["if"])

    def test_the_job_that_holds_the_write_token_installs_nothing_and_runs_no_repository_code(self):
        """The repack (apt, pip, tools/build_distribution.py, cmake -P) runs with a read-only token; the
        publish job, which holds contents: write, id-token and attestations, only downloads, attests and
        uploads. A tool or script that is compromised or changed cannot reach the token (#437)."""
        data, _ = load()
        publish, repack = data["jobs"]["publish"], data["jobs"]["repack"]
        self.assertEqual(repack["permissions"], {"contents": "read"})
        for step in publish["steps"]:
            self.assertNotIn("run", step, step)
            self.assertFalse(str(step.get("uses", "")).startswith("actions/checkout@"), step)
        self.assertEqual([str(s.get("uses", "")).split("@")[0] for s in publish["steps"]],
                         ["actions/download-artifact", "actions/download-artifact", "actions/download-artifact",
                          "actions/attest", "softprops/action-gh-release"])
        text = steps_text(publish)
        for word in ("pip", "apt-get", "cmake", "tools/"):
            self.assertNotIn(word, text, word)
        repack_text = steps_text(repack)
        for word in ("pip install cmake==", "tools/build_distribution.py", "cmake/VerifyDistribution.cmake"):
            self.assertIn(word, repack_text, word)
        self.assertEqual([s["with"]["name"] for s in repack["steps"] if "with" in s and "path" in s["with"]
                          and s["with"]["path"].startswith("dist/")], ["dist-tar-zst"])

    def test_the_release_files_get_a_provenance_attestation_in_the_publish_job_only(self):
        data, _ = load()
        for name, job in data["jobs"].items():
            has_attest = "actions/attest@" in steps_text(job)
            self.assertEqual(has_attest, name == "publish", name)
            permissions = job.get("permissions", {})
            self.assertEqual(permissions.get("id-token") == "write", name == "publish", name)
        self.assertEqual(data["jobs"]["publish"]["permissions"]["attestations"], "write")

    def test_the_attestation_is_actions_attest_v4_pinned_by_sha_and_nothing_older(self):
        """The release files are attested by actions/attest at v4 (GitHub Docs, "Using artifact attestations to
        establish provenance for builds"), pinned by a full commit SHA with the version in a comment."""
        text = (REPO / ".github/workflows/build.yml").read_text()
        self.assertNotIn("attest-build-provenance", text)
        self.assertRegex(text, r"uses: actions/attest@[0-9a-f]{40} # v4(\.\d+)*\n")
        publish = load()[0]["jobs"]["publish"]
        self.assertEqual(publish["permissions"], {"contents": "write", "id-token": "write", "attestations": "write"})

    def test_the_attested_files_are_exactly_the_uploaded_files(self):
        """`gh attestation verify <file>` finds a file only if its digest is a subject of an attestation: a
        file the release receives that is not attested has nothing to verify (#450), and an attested file
        that is never uploaded is a lie. The two lists are the same set."""
        steps = load()[0]["jobs"]["publish"]["steps"]
        attest = [i for i, s in enumerate(steps) if str(s.get("uses", "")).startswith("actions/attest@")]
        uploads = [i for i, s in enumerate(steps) if str(s.get("uses", "")).startswith("softprops/action-gh-release@")]
        self.assertEqual(len(attest), 1)
        self.assertEqual(len(uploads), 1)
        subjects = lines(steps[attest[0]]["with"]["subject-path"])
        uploaded = []
        for i in uploads:
            self.assertLess(attest[0], i, "the files are attested before they are uploaded")
            uploaded += lines(steps[i]["with"]["files"])
        # The symbol files are attested to the same commit but are a workflow artifact, never a release asset.
        self.assertEqual(sorted(s for s in subjects if not s.startswith("symbols/")), sorted(uploaded))
        self.assertEqual(sorted(subjects), sorted([
            "release-assets/*.zip", "quest-asset/*.so", "symbols/*/*.dbg"]))
        self.assertNotIn("symbols", " ".join(uploaded), "the symbol files are not release assets")
        self.assertNotIn("tar.zst", " ".join(subjects + uploaded), "zips only: no .tar.zst is a release asset")
        repack_steps = load()[0]["jobs"]["repack"]["steps"]
        tar_steps = [s for s in repack_steps if s.get("with", {}).get("path") == "dist/*.tar.zst"]
        self.assertEqual([s["with"]["name"] for s in tar_steps], ["dist-tar-zst"])
        self.assertEqual([s for s in steps if s.get("with", {}).get("path") == "dist"], [],
                         "no dist zip is downloaded for the release: the release zip is the sealed one")

    def test_the_build_job_checks_and_keeps_the_symbol_files_of_the_stripped_binaries(self):
        steps = load()[0]["jobs"]["build"]["steps"]
        verify = [s for s in steps if s.get("name") == "Verify the symbol files match the stripped binaries"]
        self.assertEqual(len(verify), 1)
        self.assertIn("tools/verify_symbols.py", verify[0]["run"])
        self.assertIn("package_release.py stamp --dll", verify[0]["run"])
        self.assertIn("verify_symbols.py --tree", verify[0]["run"])
        # The Windows zip ships the stripped dist DLL, never build/mingw-release/bin/BugSplat64.dll.
        tree = [s for s in steps if s.get("name") == "Gate and assemble the release tree"]
        self.assertEqual(len(tree), 1)
        self.assertIn('--dll "$pkg/BugSplat64.dll"', tree[0]["run"])
        self.assertIn('--server-exe "$pkg/echovr_server.exe"', tree[0]["run"])
        self.assertNotIn("--server-exe build/mingw-release", tree[0]["run"])
        self.assertNotIn("--dll build/mingw-release", tree[0]["run"])
        self.assertLess(steps.index(verify[0]), steps.index(tree[0]))
        windows = [s for s in steps if s.get("name") == "Verify the Windows tree ships only stripped binaries"]
        self.assertEqual(len(windows), 1)
        self.assertIn("verify_symbols.py --tree release-stage/", windows[0]["run"])
        self.assertLess(steps.index(tree[0]), steps.index(windows[0]))
        upload = [s for s in steps if s.get("with", {}).get("name") == "symbols"]
        self.assertEqual([s["with"]["path"] for s in upload], ["dist/symbols/"])
        self.assertLess(steps.index(verify[0]), steps.index(upload[0]))

    def test_no_step_reads_or_sets_the_pre_release_flag(self):
        """Promotion changes no byte and no run: the flag is the human's. Nothing reads the event's value,
        and the uploads leave the `prerelease` input unset so the action re-sends the release's own flag
        (read at upload time, so a promotion made while a run is going is kept)."""
        data, _ = load()
        text = WORKFLOW.read_text(encoding="utf-8")
        code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        self.assertNotIn("event.release.prerelease", code)
        self.assertNotIn("isPrerelease", code)
        for name, job in data["jobs"].items():
            self.assertNotIn("prerelease", steps_text(job).lower(), name)
        steps = data["jobs"]["publish"]["steps"]
        uploads = [s for s in steps if str(s.get("uses", "")).startswith("softprops/action-gh-release@")]
        self.assertEqual(len(uploads), 1)
        for step in uploads:
            self.assertNotIn("prerelease", step["with"])
            # The one argued exception: softprops/action-gh-release PUBLISHES a reused draft after uploading
            # unless `draft: true` ("If the action reuses an existing draft release, set draft: true to keep
            # it draft; if draft is omitted, the action will publish that draft after uploading assets"). A
            # dispatch on a draft's tag must keep the draft a draft; for a release event the expression is
            # false, which is the action's default (nothing about the release's state changes). Nothing else
            # may ever be set here.
            self.assertEqual(step["with"].get("draft"), "${{ github.event_name == 'workflow_dispatch' }}")
            self.assertTrue(step["with"]["fail_on_unmatched_files"])
        for name, job in data["jobs"].items():
            for step in job["steps"]:
                if step in uploads:
                    continue
                self.assertNotIn("draft", step.get("with", {}), f"{name}: {step.get('name', step.get('uses'))}")

    def test_no_secret_is_added(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(sorted(set(re.findall(r"secrets\.([A-Za-z0-9_]+)", text))), ["GITHUB_TOKEN"])

    # --- the release version step, executed -----------------------------------------------------------

    def run_step(self, job, step_id, **env):
        data, _ = load()
        step = next(s for s in data["jobs"][job]["steps"] if s.get("id") == step_id)
        with tempfile.TemporaryDirectory(prefix="workflow-step-") as tmp:
            out = Path(tmp) / "out"
            out.write_text("")
            clean = {k: v for k, v in os.environ.items() if not k.startswith(("GITHUB_", "EVENT", "TAG"))}
            clean.update({"EVENT": "workflow_dispatch", "TAG": "", "GITHUB_OUTPUT": str(out)})
            clean.update(env)
            result = subprocess.run(["bash", "-c", step["run"]], env=clean, capture_output=True, text=True, cwd=REPO)
            outputs = dict(line.split("=", 1) for line in out.read_text().splitlines())
            return result.returncode, outputs, result.stdout, result.stderr

    def test_the_version_is_exactly_the_tag_for_a_release_event(self):
        rc, out, _, _ = self.run_step("build", "release", EVENT="release", TAG="v5.0.0",
                                      GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v5.0.0")
        self.assertEqual((rc, out.get("version")), (0, "5.0.0"))
        rc, out, _, _ = self.run_step("build", "release", EVENT="release", TAG="v12.3.45",
                                      GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v12.3.45")
        self.assertEqual((rc, out.get("version")), (0, "12.3.45"))

    def test_the_step_logs_the_ref_and_the_version_it_decided(self):
        """The version step is an Actions-log-only record (artifacts keep 7 days); its lines are asserted."""
        rc, _, out, _ = self.run_step("build", "release", EVENT="release", TAG="v5.0.0",
                                      GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v5.0.0")
        self.assertEqual(rc, 0)
        self.assertIn("ref: type=tag name=v5.0.0 event=release", out)
        self.assertIn("release version: '5.0.0'", out)
        rc, _, out, _ = self.run_step("build", "release", GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="main")
        self.assertIn("release version: '' (empty: a branch build, which stamps a development version)", out)

    def test_a_dispatch_on_a_release_tag_is_a_dry_run_with_the_same_version(self):
        rc, out, _, _ = self.run_step("build", "release", GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v5.0.1")
        self.assertEqual((rc, out.get("version")), (0, "5.0.1"))

    def test_a_branch_build_has_no_release_version(self):
        rc, out, _, _ = self.run_step("build", "release", GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="main")
        self.assertEqual((rc, out.get("version")), (0, ""))

    def test_a_tag_that_is_not_vx_y_z_fails_with_one_line(self):
        for tag in ("v5.0", "v5.0.0-beta.1", "v4.0.0-rc.1", "5.0.0", "release-5", "v5.0.0.1", "vx.y.z",
                    "v05.0.0", "v5.00.0", "v5.0.00", "v5.0.0+x", "v5.0.0-rc.1", "V5.0.0", "v5.0.0 "):
            with self.subTest(tag=tag):
                rc, out, _, err = self.run_step("build", "release", EVENT="release", TAG=tag,
                                                GITHUB_REF_TYPE="tag", GITHUB_REF_NAME=tag)
                self.assertEqual(rc, 1)
                self.assertEqual(len(err.strip().splitlines()), 1, err)
                self.assertIn("is not vX.Y.Z", err)
                self.assertNotIn("version", out)
                rc, out, _, err = self.run_step("build", "release", GITHUB_REF_TYPE="tag", GITHUB_REF_NAME=tag)
                self.assertEqual(rc, 1)

    def test_a_release_whose_ref_is_not_its_tag_fails(self):
        rc, _, _, err = self.run_step("build", "release", EVENT="release", TAG="v5.0.0",
                                      GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="main")
        self.assertEqual(rc, 1)
        self.assertIn("but the ref is", err)

    # --- the guard: a release that already carries its assets is not rebuilt --------------------------

    def guard(self, event="release", tag="v5.0.0", assets=TWO, view_rc=0):
        with tempfile.TemporaryDirectory(prefix="guard-") as tmp:
            bindir = Path(tmp) / "bin"
            bindir.mkdir()
            gh = bindir / "gh"
            gh.write_text(FAKE_GH)
            gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
            log = Path(tmp) / "gh.log"
            env = {"PATH": f"{bindir}:{os.environ['PATH']}", "FAKE_GH_LOG": str(log),
                   "FAKE_ASSETS": "\n".join(assets) + "\n", "FAKE_VIEW_RC": str(view_rc)}
            rc, out, stdout, err = self.run_step("guard", "check", GITHUB_REPOSITORY="EchoTools/nevr-runtime",
                                                 EVENT=event, TAG=tag, **env)
            return rc, out.get("built"), (log.read_text().splitlines() if log.exists() else [])

    def test_a_release_with_both_assets_is_not_rebuilt(self):
        rc, built, calls = self.guard()
        self.assertEqual((rc, built), (0, "true"))
        self.assertEqual(len(calls), 1)
        self.assertIn("release view v5.0.0", calls[0])

    def test_a_release_missing_any_one_asset_is_built(self):
        for missing in TWO:
            with self.subTest(missing=missing):
                rc, built, _ = self.guard(assets=[a for a in TWO if a != missing])
                self.assertEqual((rc, built), (0, "false"))

    def test_a_release_with_no_assets_is_built(self):
        self.assertEqual(self.guard(assets=[])[:2], (0, "false"))

    def test_other_versions_assets_do_not_count(self):
        other = [a.replace("5.0.0", "4.9.9") for a in TWO]
        self.assertEqual(self.guard(assets=other)[:2], (0, "false"))

    def test_a_failing_listing_fails_the_job_it_does_not_mean_rebuild(self):
        rc, built, _ = self.guard(view_rc=1)
        self.assertNotEqual(rc, 0)
        self.assertIsNone(built)

    def test_a_dispatch_never_asks_github_and_is_never_built_already(self):
        rc, built, calls = self.guard(event="workflow_dispatch", tag="")
        self.assertEqual((rc, built, calls), (0, "false", []))

    # --- the packaging path ----------------------------------------------------------------------------

    def test_the_build_asserts_the_stamped_version_right_after_building(self):
        data, _ = load()
        names = [step.get("name", "") for step in data["jobs"]["build"]["steps"]]
        self.assertIn("Assert the stamped version", names)
        self.assertLess(names.index("Build"), names.index("Assert the stamped version"))
        self.assertLess(names.index("Assert the stamped version"), names.index("Gate and assemble the release tree"))
        self.assertLess(names.index("Release version"), names.index("Configure CMake"))
        step = data["jobs"]["build"]["steps"][names.index("Assert the stamped version")]
        self.assertIn("tools/package_release.py stamp", step["run"])
        self.assertIn('--commit "$GITHUB_SHA"', step["run"])

    def test_verify_is_skipped_only_on_a_tag_and_no_input_skips_it(self):
        # #1008: a tag build skips Verify (every commit on main passed its own checks); a branch or main dispatch
        # runs it, and no workflow_dispatch input can turn it off.
        data, on = load()
        step = next(s for s in data["jobs"]["build"]["steps"] if s.get("name") == "Verify")
        self.assertEqual(step.get("if"), "github.ref_type != 'tag'")
        self.assertFalse((on.get("workflow_dispatch") or {}).get("inputs"))

    def test_the_release_is_gated_then_sealed_with_no_apk(self):
        data, _ = load()
        build = steps_text(data["jobs"]["build"])
        self.assertIn("tools/package_release.py tree", build)
        self.assertIn("unsigned-release-tree", build)
        seal = steps_text(data["jobs"]["seal"])
        runs = "\n".join(step.get("run", "") for step in data["jobs"]["seal"]["steps"])
        seal_command = next(line for line in runs.splitlines() if line.startswith("python3 tools/package_release.py seal"))
        self.assertNotIn("--apk", seal_command)
        self.assertNotIn("gh release", seal)
        self.assertIn("sha256sum -c SHA256SUMS", seal)
        self.assertIn("unsigned-release-tree", seal)
        self.assertNotIn("packages-unsigned", seal)
        self.assertIn("release-unsigned", seal)
        self.assertIn("staged/release/nevr-runtime-v*-windows)", seal)
        self.assertIn("unzip -q", seal)
        self.assertNotIn("release-assets && sha256sum", seal, "no top-level SHA256SUMS is checked: none is written")
        self.assertFalse((REPO / "tools" / "rc_release_apk.sh").exists())

    def test_the_build_writes_the_public_defaults_before_it_configures(self):
        data, _ = load()
        names = [step.get("name", "") for step in data["jobs"]["build"]["steps"]]
        self.assertLess(names.index("Write the public client defaults"), names.index("Configure CMake"))

    def test_actionlint_accepts_the_workflow_when_installed(self):
        actionlint = shutil.which("actionlint")
        if actionlint is None:
            self.skipTest("actionlint is not installed")
        result = subprocess.run([actionlint, str(WORKFLOW)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
