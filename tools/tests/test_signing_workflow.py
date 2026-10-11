"""The release workflow (build.yml): structure and the small scripts it runs; nothing is published here.

A release is a plain semver tag plus GitHub's pre-release flag. Signing is stubbed until the policy signer
exists. These tests pin that the packaging path is one path for every release event, that nothing in it
reads the pre-release flag (promotion changes no byte), that a release which already carries its assets is
not rebuilt, and that the stub is honest."""

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

FIVE = ["nevr-runtime-v5.0.0-windows.zip", "SHA256SUMS", "RELEASE-NOTES.md", "nevr-runtime-v5.0.0.zip",
        "nevr-runtime-v5.0.0-lite.zip"]


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

    def test_the_sign_job_is_a_named_stub_that_passes_the_files_through(self):
        data, _ = load()
        sign = data["jobs"]["sign"]
        self.assertEqual(sign["name"], "sign: stubbed, policy signer not built yet")
        self.assertIn("PLACEHOLDER: the signer dispatch goes here", WORKFLOW.read_text(encoding="utf-8"))
        text = steps_text(sign)
        self.assertIn("packages-unsigned", text)
        self.assertNotRegex(WORKFLOW.read_text(encoding="utf-8"), r"name: signed-")

    def test_the_job_graph_is_guard_build_sign_seal_repack_publish(self):
        jobs = load()[0]["jobs"]
        self.assertEqual(list(jobs), ["guard", "build", "sign", "seal", "repack", "publish"])
        self.assertEqual(jobs["build"]["needs"], "guard")
        self.assertIn("needs.guard.outputs.built != 'true'", jobs["build"]["if"])
        self.assertEqual(sorted(jobs["seal"]["needs"]), ["build", "sign"])
        self.assertEqual(sorted(jobs["repack"]["needs"]), ["seal", "sign"])
        self.assertEqual(sorted(jobs["publish"]["needs"]), ["repack", "seal", "sign"])
        self.assertEqual(jobs["sign"]["needs"], "build")
        for name in ("sign", "seal", "repack", "publish"):
            self.assertNotIn("rc", name)

    def test_nothing_is_published_without_a_release_event(self):
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
                         ["actions/download-artifact", "actions/download-artifact",
                          "actions/attest-build-provenance", "softprops/action-gh-release",
                          "softprops/action-gh-release"])
        text = steps_text(publish)
        for word in ("pip", "apt-get", "cmake", "tools/"):
            self.assertNotIn(word, text, word)
        repack_text = steps_text(repack)
        for word in ("pip install cmake==", "tools/build_distribution.py", "cmake/VerifyDistribution.cmake"):
            self.assertIn(word, repack_text, word)
        self.assertEqual([s["with"]["name"] for s in repack["steps"] if "with" in s and "path" in s["with"]
                          and s["with"]["path"].startswith("dist/")], ["dist-tar-zst", "dist-zips"])

    def test_the_release_files_get_a_provenance_attestation_in_the_publish_job_only(self):
        data, _ = load()
        for name, job in data["jobs"].items():
            has_attest = "actions/attest-build-provenance" in steps_text(job)
            self.assertEqual(has_attest, name == "publish", name)
            permissions = job.get("permissions", {})
            self.assertEqual(permissions.get("id-token") == "write", name == "publish", name)
        self.assertEqual(data["jobs"]["publish"]["permissions"]["attestations"], "write")

    def test_the_attested_files_are_exactly_the_uploaded_files(self):
        """`gh attestation verify <file>` finds a file only if its digest is a subject of an attestation: a
        file the release receives that is not attested has nothing to verify (#450), and an attested file
        that is never uploaded is a lie. The two lists are the same set."""
        steps = load()[0]["jobs"]["publish"]["steps"]
        attest = [i for i, s in enumerate(steps) if str(s.get("uses", "")).startswith("actions/attest-build-provenance@")]
        uploads = [i for i, s in enumerate(steps) if str(s.get("uses", "")).startswith("softprops/action-gh-release@")]
        self.assertEqual(len(attest), 1)
        self.assertEqual(len(uploads), 2)
        subjects = lines(steps[attest[0]]["with"]["subject-path"])
        uploaded = []
        for i in uploads:
            self.assertLess(attest[0], i, "the files are attested before they are uploaded")
            uploaded += lines(steps[i]["with"]["files"])
        self.assertEqual(sorted(subjects), sorted(uploaded))
        self.assertEqual(sorted(subjects), sorted([
            "dist/*.zip", "release-assets/*.zip", "release-assets/SHA256SUMS", "release-assets/RELEASE-NOTES.md"]))
        self.assertNotIn("tar.zst", " ".join(subjects + uploaded), "zips only: no .tar.zst is a release asset")
        repack_steps = load()[0]["jobs"]["repack"]["steps"]
        tar_steps = [s for s in repack_steps if s.get("with", {}).get("path") == "dist/*.tar.zst"]
        self.assertEqual([s["with"]["name"] for s in tar_steps], ["dist-tar-zst"])
        self.assertEqual([s["with"]["name"] for s in steps if s.get("with", {}).get("path") == "dist"], ["dist-zips"])

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
        self.assertEqual(len(uploads), 2)
        for step in uploads:
            self.assertNotIn("prerelease", step["with"])
            self.assertNotIn("draft", step["with"])
            self.assertTrue(step["with"]["fail_on_unmatched_files"])

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

    def guard(self, event="release", tag="v5.0.0", assets=FIVE, view_rc=0):
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

    def test_a_release_with_all_five_assets_is_not_rebuilt(self):
        rc, built, calls = self.guard()
        self.assertEqual((rc, built), (0, "true"))
        self.assertEqual(len(calls), 1)
        self.assertIn("release view v5.0.0", calls[0])

    def test_a_release_missing_any_one_asset_is_built(self):
        for missing in FIVE:
            with self.subTest(missing=missing):
                rc, built, _ = self.guard(assets=[a for a in FIVE if a != missing])
                self.assertEqual((rc, built), (0, "false"))

    def test_a_release_with_no_assets_is_built(self):
        self.assertEqual(self.guard(assets=[])[:2], (0, "false"))

    def test_other_versions_assets_do_not_count(self):
        other = [a.replace("5.0.0", "4.9.9") for a in FIVE]
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

    def test_verify_always_runs_there_is_no_input_that_skips_it(self):
        data, _ = load()
        step = next(s for s in data["jobs"]["build"]["steps"] if s.get("name") == "Verify")
        self.assertNotIn("if", step)

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
        self.assertIn("packages-unsigned", seal)
        self.assertIn("release-unsigned", seal)
        self.assertIn("staged/release/nevr-runtime-v*-windows", seal)
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
