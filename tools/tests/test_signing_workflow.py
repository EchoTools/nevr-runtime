"""The release workflow's signing path (build.yml): structure only, nothing is run or published.

Signing is stubbed until the policy signer exists; these tests pin that the stub is honest (UNSIGNED
everywhere, no Windows runner, no cloud login) and that the release candidate path is wired."""

import re
import shutil
import subprocess
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "build.yml"


def load():
    data = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    # PyYAML reads the bare key `on` as True.
    return data, data.get("on", data.get(True))


def steps_text(job: dict) -> str:
    return yaml.safe_dump(job.get("steps", []))


class SigningWorkflowTest(unittest.TestCase):
    def test_a_dry_run_is_a_dispatch_input_that_defaults_to_empty(self):
        _, triggers = load()
        rc_number = triggers["workflow_dispatch"]["inputs"]["rc_number"]
        self.assertEqual(rc_number["default"], "")
        self.assertNotIn("sign_test", triggers["workflow_dispatch"]["inputs"])

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

    def test_the_sign_job_is_a_named_stub_that_passes_the_files_through_unsigned(self):
        data, _ = load()
        sign = data["jobs"]["sign"]
        self.assertEqual(sign["name"], "sign: stubbed, policy signer not built yet")
        text = steps_text(sign)
        self.assertIn("PLACEHOLDER: the signer dispatch goes here", WORKFLOW.read_text(encoding="utf-8"))
        self.assertIn("UNSIGNED passthrough", text)
        # The artifact it hands on says what it is.
        self.assertIn("packages-unsigned", text)
        self.assertNotRegex(WORKFLOW.read_text(encoding="utf-8"), r"name: signed-")

    def test_nothing_is_published_without_a_release_event(self):
        data, _ = load()
        for name, job in data["jobs"].items():
            uses_release_action = "softprops/action-gh-release" in steps_text(job)
            self.assertEqual(uses_release_action, name == "publish", name)
        publish = data["jobs"]["publish"]
        self.assertIn("github.event_name == 'release'", publish["if"])
        self.assertEqual(sorted(publish["needs"]), ["rc-seal", "sign"])

    def test_the_release_files_get_a_provenance_attestation_in_the_publish_job_only(self):
        data, _ = load()
        for name, job in data["jobs"].items():
            has_attest = "actions/attest-build-provenance" in steps_text(job)
            self.assertEqual(has_attest, name == "publish", name)
            permissions = job.get("permissions", {})
            self.assertEqual(permissions.get("id-token") == "write", name == "publish", name)
        publish = data["jobs"]["publish"]
        self.assertEqual(publish["permissions"]["attestations"], "write")

    def test_every_published_release_file_is_an_attestation_subject(self):
        """`gh attestation verify <file>` finds a file only if its digest is a subject of an attestation, so a
        file the release receives that is not listed in the attest step has nothing to verify (#450)."""
        publish = load()[0]["jobs"]["publish"]
        steps = publish["steps"]

        def lines(value: str) -> list:
            return [line.strip() for line in value.splitlines() if line.strip()]

        attest = [i for i, step in enumerate(steps) if str(step.get("uses", "")).startswith("actions/attest-build-provenance@")]
        uploads = [i for i, step in enumerate(steps) if str(step.get("uses", "")).startswith("softprops/action-gh-release@")]
        self.assertEqual(len(attest), 1, "one attestation step covers every published file")
        self.assertGreaterEqual(len(uploads), 2)
        subjects = lines(steps[attest[0]]["with"]["subject-path"])
        for i in uploads:
            self.assertLess(attest[0], i, "the files are attested before they are uploaded")
            for pattern in lines(steps[i]["with"]["files"]):
                self.assertIn(pattern, subjects, f"{pattern} is uploaded to the release but not attested")
        # The release candidate's public set: the zip, the checksums and the notes.
        for pattern in ("rc-dist/*.zip", "rc-dist/SHA256SUMS", "rc-dist/RELEASE-NOTES.md"):
            self.assertIn(pattern, subjects)

    def test_no_secret_is_added(self):
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(sorted(set(re.findall(r"secrets\.([A-Za-z0-9_]+)", text))), ["GITHUB_TOKEN"])

    def test_the_release_candidate_is_gated_then_sealed_unsigned(self):
        data, _ = load()
        build = steps_text(data["jobs"]["build"])
        self.assertIn("tools/package_rc.py tree", build)
        self.assertIn("-DNEVR_RC_LABEL=", build)
        seal_job = data["jobs"]["rc-seal"]
        self.assertEqual(sorted(seal_job["needs"]), ["build", "sign"])
        seal = steps_text(seal_job)
        self.assertIn("tools/package_rc.py seal", seal)
        runs = "\n".join(step.get("run", "") for step in seal_job["steps"])
        seal_command = next(line for line in runs.splitlines() if line.startswith("python3 tools/package_rc.py seal"))
        self.assertNotIn("--require-signed", seal_command, "nothing is signed yet: the seal must say UNSIGNED")
        self.assertIn("sha256sum -c SHA256SUMS", seal)
        self.assertIn("packages-unsigned", seal)
        self.assertIn("release-candidate-unsigned", seal)

    def test_a_release_candidates_apk_is_listed_then_fetched_and_a_failure_fails_the_job(self):
        data, _ = load()
        text = steps_text(data["jobs"]["rc-seal"])
        self.assertIn("tools/rc_release_apk.sh", text)
        self.assertNotIn("2>/dev/null", text, "a failed APK listing or download must not be swallowed")
        self.assertNotIn("gh release download", text, "the download goes through rc_release_apk.sh")

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
