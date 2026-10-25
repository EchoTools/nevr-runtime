"""A release is one Windows zip and the Quest library: the list, the build, and RELEASING.md agree."""
import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
NAMES = REPO / "tools" / "release_asset_names.sh"
AUDIENCE = ("windows", "quest")


def listed(version="5.5.0"):
    out = subprocess.run(["bash", "-c", f'source "{NAMES}"; release_asset_names {version}'],
                         capture_output=True, text=True, check=True).stdout
    return out.split("\n")[:-1]


class ReleaseAssetsTest(unittest.TestCase):
    def test_the_release_lists_exactly_these_two_names(self):
        self.assertEqual(listed(), ["nevr-runtime-v5.5.0-windows.zip", "nevr-runtime-v5.5.0-quest-libovrplatformloader.so"])

    def test_no_checksum_or_notes_file_and_no_second_windows_zip_is_a_release_asset(self):
        names = listed()
        for gone in ("SHA256SUMS", "RELEASE-NOTES.md", "nevr-runtime-v5.5.0-windows-player.zip",
                     "nevr-runtime-v5.5.0-windows-server.zip", "nevr-runtime-v5.5.0.zip"):
            self.assertNotIn(gone, names)
        for name in names:
            self.assertTrue(any(f"-{a}" in name for a in AUDIENCE), f"{name} names no audience")

    def test_the_packager_makes_the_listed_zip_and_the_dist_target_is_not_a_release_asset(self):
        packager = (REPO / "tools" / "package_release.py").read_text()
        self.assertIn('f"{base}-windows.zip"', packager)
        self.assertNotIn("-windows-player", packager)
        cmake = (REPO / "CMakeLists.txt").read_text()
        self.assertIn('set(DIST_NAME "nevr-runtime-v${PROJECT_VERSION}-windows-server")', cmake)
        self.assertNotIn("windows-player", cmake)

    def test_the_workflow_globs_name_the_one_windows_tree_and_ship_no_dist_zip(self):
        build = (REPO / ".github/workflows/build.yml").read_text()
        self.assertEqual(len(re.findall(r"nevr-runtime-v\*-windows(?!-)", build)), 2)
        self.assertNotIn("windows-player", build)
        self.assertNotIn("dist-zips", build)
        subjects = build.split("subject-path: |", 1)[1].split("\n\n", 1)[0].split()
        self.assertEqual(subjects, ["release-assets/*.zip", "quest-asset/*.so", "symbols/*/*.dbg"])
        files = build.split("fail_on_unmatched_files: true\n        files: |", 1)[1].split("env:", 1)[0].split()
        self.assertEqual(files, ["release-assets/*.zip", "quest-asset/*.so"])
        self.assertNotIn("release-assets/SHA256SUMS", build)
        self.assertNotIn("release-assets/RELEASE-NOTES.md", build)

    def test_releasing_md_lists_each_asset_and_agents_md_points_at_it(self):
        text = (REPO / "RELEASING.md").read_text()
        for name in listed("X.Y.Z"):
            self.assertIn(f"`{name}`", text, f"RELEASING.md does not list {name}")
        agents = (REPO / "AGENTS.md").read_text()
        self.assertIn("Every release follows RELEASING.md.", agents)

    def test_releasing_md_carries_the_three_release_steps_agents_md_carries(self):
        releasing = (REPO / "RELEASING.md").read_text()
        for step in ("just release-preflight",
                     'git tag -a vX.Y.Z -m "X.Y.Z" && git push origin vX.Y.Z',
                     "gh workflow run build.yml --ref vX.Y.Z"):
            self.assertIn(step, releasing)
        agents = (REPO / "AGENTS.md").read_text()
        for step in ("just release-preflight", 'git tag -a v<x.y.z> -m "<x.y.z>" && git push origin v<x.y.z>',
                     "gh workflow run build.yml --ref v<x.y.z>"):
            self.assertIn(step, agents)

    def test_releasing_md_checks_locally_before_the_tag_and_rehearses_nothing_on_actions(self):
        # Actions minutes are billed: the workflow runs once, on the real tag. No scratch tag, no dispatch before it.
        text = (REPO / "RELEASING.md").read_text()
        before = text.split("## Before the tag", 1)[1].split("## Cut the draft", 1)[0]
        self.assertIn("just package-dev", before)
        self.assertIn("`just verify` has run on this tip", before)
        self.assertIn("PC login test passes", before)
        self.assertNotIn("gh workflow run", before)
        self.assertNotIn("v0.0.<issue>", before)
        dispatches = [l for l in text.splitlines() if l.startswith("gh workflow run build.yml")]
        self.assertEqual(dispatches, ["gh workflow run build.yml --ref vX.Y.Z                    # 4. build, attest, attach the two files; it stays a draft"])
        sign = text.split("## Sign", 1)[1].split("Check a signed file as", 1)[0]
        self.assertIn("-f dry_run=false", sign)
        self.assertNotIn("-f dry_run=true", sign)

    def test_releasing_md_creates_a_titled_draft_and_publishes_from_it(self):
        text = (REPO / "RELEASING.md").read_text()
        self.assertIn("gh release create vX.Y.Z --title vX.Y.Z --draft --prerelease --verify-tag", text)
        self.assertIn("gh release edit vX.Y.Z --draft=false", text)
        self.assertIn("gh attestation verify", text)
        self.assertIn("Enable release immutability", text)

    def test_every_gh_release_create_is_a_titled_draft_and_agents_md_publishes_from_it(self):
        # Release immutability is on: a release created as published is locked before the build attaches files.
        for name in ("AGENTS.md", "RELEASING.md"):
            text = (REPO / name).read_text(encoding="utf-8")
            creates = [l for l in text.splitlines() if "&& gh release create " in l]
            self.assertTrue(creates, name)
            for line in creates:
                self.assertRegex(line, r"--title v\S+ --draft --prerelease --verify-tag", f"{name}: {line}")
            self.assertIn("gh release edit v", text)
            self.assertIn("--draft=false", text, name)

    def test_every_workflow_gh_release_create_is_a_draft_prerelease_with_a_verified_tag(self):
        # Release immutability is on: a release created as published is locked before any file is attached, so a
        # workflow that creates one without --draft publishes an empty release nobody can fix.
        found = 0
        for path in sorted((REPO / ".github/workflows").glob("*.yml")):
            text = path.read_text(encoding="utf-8").replace("\\\n", " ")
            for line in text.splitlines():
                if "gh release create" not in line or line.lstrip().startswith("#"):
                    continue
                found += 1
                for flag in ("--draft", "--prerelease", "--verify-tag"):
                    self.assertIn(flag, line, f"{path.name}: {line.strip()}")
        self.assertGreaterEqual(found, 1)

    def test_releasing_md_covers_the_failed_draft_the_zips_own_sums_and_the_release_body_after_signing(self):
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        self.assertIn("A draft that fails its checks is deleted and cut again", releasing)
        self.assertIn("gh release delete vX.Y.Z", releasing)
        self.assertIn("The signer replaces the zip. The `SHA256SUMS` inside it", releasing)
        self.assertIn("covers the unsigned build output CI attested", releasing)
        body = releasing.split("## Write the release body", 1)[1].split("## Publish", 1)[0]
        self.assertIn("python3 tools/package_release.py notes --assets", body)
        self.assertIn("gh release edit vX.Y.Z --notes-file", body)
        self.assertIn("after the last signing pass", body)
        self.assertNotIn("top-level `SHA256SUMS` gets", releasing)

    def test_releasing_md_says_the_attestation_loop_fails_for_the_signed_zip_and_passes_for_the_quest_library(self):
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        after_signing = " ".join(releasing.split("## Sign", 1)[1].split("## Write the release body", 1)[0].split())
        self.assertIn('The loop under "Check the draft" exits non-zero for the signed zip (`HTTP 404: Not Found` from '
                      "the attestations API)", after_signing)
        self.assertIn("still exits 0 for the Quest library, which the signer does not replace", after_signing)
        self.assertIn("not a bad draft", after_signing)

    def test_readme_issue_template_and_justfile_carry_the_asset_names_and_the_body_holds_the_sums(self):
        self.assertIn("-windows.zip", (REPO / "README.md").read_text(encoding="utf-8"))
        self.assertIn("-windows.zip", (REPO / ".github/ISSUE_TEMPLATE/beta-bug.yml").read_text(encoding="utf-8"))
        self.assertNotIn("windows-player", (REPO / "README.md").read_text(encoding="utf-8"))
        self.assertIn("-windows-server) and its symbol files", (REPO / "justfile").read_text(encoding="utf-8"))
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        self.assertIn("There is no top-level `SHA256SUMS` and no `RELEASE-NOTES.md` asset", releasing)
        self.assertIn("a table\nof the SHA-256 of each of the two assets", releasing)
        self.assertIn("Quest APKs are published outside GitHub", releasing)
        self.assertNotIn("echo.taxi", releasing)
        self.assertIn("The zip has no `plugins/` entry", releasing)
        self.assertNotIn("an empty `plugins/`", releasing)


if __name__ == "__main__":
    unittest.main()
