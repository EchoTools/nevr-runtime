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

    def test_every_release_upload_step_ships_only_the_windows_zip_and_the_quest_library(self):
        # A game-derived pack never rides on a release: each action-gh-release `files:` block names only the zip
        # and the Quest library, and no workflow line uploads a pack or content with `gh release upload`.
        blocks = 0
        for path in sorted((REPO / ".github/workflows").glob("*.yml")):
            for step in re.split(r"(?m)^(?=\s*- )", path.read_text(encoding="utf-8")):
                if not re.search(r"(?m)^\s*(- )?uses:\s*softprops/action-gh-release@", step):
                    continue
                lines = step.splitlines()
                for i, line in enumerate(lines):
                    m = re.match(r"^(\s*)files:\s*(.*)$", line)
                    if not m:
                        continue
                    blocks += 1
                    indent = len(m.group(1))
                    globs = m.group(2).split()
                    if m.group(2).strip() == "|":
                        globs = []
                        for body in lines[i + 1:]:
                            if body.strip() == "":
                                continue
                            if len(body) - len(body.lstrip()) <= indent:
                                break
                            globs.extend(body.split())
                    self.assertTrue(globs, f"{path.name}: files block is empty")
                    for glob in globs:
                        self.assertTrue(glob.endswith("release-assets/*.zip") or glob.endswith("quest-asset/*.so"),
                                        f"{path.name}: files glob {glob} is not the zip or the Quest library")
        self.assertGreaterEqual(blocks, 1, "no action-gh-release files block was found to check")
        for path in sorted((REPO / ".github/workflows").glob("*.yml")):
            for line in path.read_text(encoding="utf-8").splitlines():
                if "gh release upload" in line:
                    self.assertFalse(re.search(r"pack|content", line), f"{path.name}: {line.strip()}")

    def test_releasing_md_lists_each_asset_and_agents_md_points_at_it(self):
        text = (REPO / "RELEASING.md").read_text()
        for name in listed("X.Y.Z"):
            self.assertIn(f"`{name}`", text, f"RELEASING.md does not list {name}")
        agents = (REPO / "AGENTS.md").read_text()
        self.assertIn("Every release follows RELEASING.md.", agents)

    def test_releasing_md_carries_the_preflight_and_the_one_step_publish_and_agents_md_carries_its_three_steps(self):
        # RELEASING.md states the flow the release is going to: the preflight, then one `gh release create` that makes the
        # tag, the release and its assets. KNOWN-WRONG #1865: AGENTS.md and the workflows still tag, cut a draft and
        # dispatch the build by hand; when #1865 lands AGENTS.md follows RELEASING.md and the second half goes.
        releasing = (REPO / "RELEASING.md").read_text()
        for step in ("just release-preflight", "gh release create vX.Y.Z --prerelease <staged files…>"):
            self.assertIn(step, releasing)
        agents = (REPO / "AGENTS.md").read_text()
        for step in ("just release-preflight", 'git tag -a v<x.y.z> -m "<x.y.z>" && git push origin v<x.y.z>',
                     "gh workflow run build.yml --ref v<x.y.z>"):
            self.assertIn(step, agents)

    def test_releasing_md_touches_nothing_public_before_the_publish_and_rehearses_nothing_on_actions(self):
        # Actions minutes are billed: no scratch tag, no rehearsal. The checks run on the staged set and the device and
        # gameplay tests run after the publish (the full suite and the PC login test run on main after the cut).
        text = (REPO / "RELEASING.md").read_text()
        flat = " ".join(text.split())
        self.assertIn("Steps 1 to 3 touch nothing public. Step 4 is the only point at which a tag or a release is created", flat)
        self.assertIn("No device or gameplay test runs before the publish", flat)
        self.assertIn("release tests run after the release is cut, signed and published", flat)
        self.assertNotIn("`just verify` has run on this tip", text)
        self.assertNotIn("PC login test passes", text)
        self.assertNotIn("v0.0.<issue>", text)
        self.assertEqual([l for l in text.splitlines() if l.startswith("gh workflow run build.yml")], [])
        sign = text.split("## Sign", 1)[1].split("Check a signed file as", 1)[0]
        self.assertNotIn("-f dry_run=true", sign)
        self.assertIn("`dry_run=true`, which writes nothing", " ".join(sign.split()))

    def test_releasing_md_publishes_a_titled_prerelease_in_one_step_and_names_the_checks_and_the_setting(self):
        text = (REPO / "RELEASING.md").read_text()
        flat = " ".join(text.split())
        self.assertIn("`gh release create vX.Y.Z --prerelease <files…>` creates the tag, the release and all its assets in one step", flat)
        self.assertIn("A release is published as a pre-release and is never left as a draft", flat)
        self.assertIn("**GitHub release title:** the tag, `vX.Y.Z`, and nothing else.", text)
        self.assertIn("gh attestation verify", text)
        self.assertIn("Enable release immutability", text)

    def test_every_gh_release_create_in_releasing_md_is_a_prerelease_with_its_files_and_never_a_draft(self):
        # Release immutability is on: a release created as published is locked before any file is attached, so the one
        # command that creates it carries the files. RELEASING.md's end state has no draft (KNOWN-WRONG #1865).
        text = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        creates = [l for l in text.splitlines() if "gh release create " in l]
        self.assertGreaterEqual(len(creates), 2, creates)
        for line in creates:
            self.assertIn("--prerelease", line, line)
            self.assertRegex(line, r"(<staged files…>|<files…>)", line)
            self.assertNotIn("--draft", line, line)
        self.assertNotIn("gh release edit vX.Y.Z --draft=false", text)

    def test_agents_md_still_cuts_a_titled_draft_and_publishes_from_it(self):
        # KNOWN-WRONG #1865: AGENTS.md describes what the workflows do today (a titled draft, published by edit). It
        # follows RELEASING.md when #1865 lands, and this test goes with it.
        text = (REPO / "AGENTS.md").read_text(encoding="utf-8")
        creates = [l for l in text.splitlines() if "&& gh release create " in l]
        self.assertTrue(creates)
        for line in creates:
            self.assertRegex(line, r"--title v\S+ --draft --prerelease --verify-tag", line)
        self.assertIn("gh release edit v", text)
        self.assertIn("--draft=false", text)

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

    def test_releasing_md_covers_the_failed_check_the_zips_own_sums_and_the_release_body_after_signing(self):
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        flat = " ".join(releasing.split())
        self.assertIn("A failed check deletes the staging directory, so no half-built release is ever visible.", flat)
        row = next(l for l in releasing.splitlines() if l.startswith("| 3f |"))
        self.assertIn("delete the staging directory", row)
        self.assertIn("gh release delete vX.Y.Z", releasing)
        sign = releasing.split("## Sign", 1)[1].split("\n## ", 1)[0]
        self.assertIn("`sha256sum -c SHA256SUMS` inside the unzipped signed zip, in a new, empty directory", " ".join(sign.split()))
        self.assertIn("covers the unsigned build output CI attested", releasing)
        body = releasing.split("## Write the release body", 1)[1].split("\n## ", 1)[0]
        self.assertIn("python3 tools/package_release.py notes --assets", body)
        self.assertIn("gh release edit vX.Y.Z --notes-file", body)
        self.assertIn("after the last signing pass", " ".join(body.split()))
        self.assertNotIn("top-level `SHA256SUMS` gets", releasing)

    def test_releasing_md_says_the_attestation_check_fails_for_the_signed_zip_and_passes_for_the_quest_library(self):
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        attestation = " ".join(releasing.split("## Attestation", 1)[1].split("\n## ", 1)[0].split())
        self.assertIn("`gh attestation verify` exits non-zero for the signed zip (`HTTP 404: Not Found` from the attestations API)",
                      attestation)
        self.assertIn("still exits 0 for the Quest library, which the signer does not replace", attestation)
        self.assertIn("not a bad release", attestation)

    def test_readme_issue_template_and_justfile_carry_the_asset_names_and_the_body_holds_the_sums(self):
        self.assertIn("-windows.zip", (REPO / "README.md").read_text(encoding="utf-8"))
        self.assertIn("-windows.zip", (REPO / ".github/ISSUE_TEMPLATE/beta-bug.yml").read_text(encoding="utf-8"))
        self.assertNotIn("windows-player", (REPO / "README.md").read_text(encoding="utf-8"))
        self.assertIn("-windows-server) and its symbol files", (REPO / "justfile").read_text(encoding="utf-8"))
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        self.assertIn("There is no top-level `SHA256SUMS` and no `RELEASE-NOTES.md` asset", releasing)
        self.assertIn("It holds no hash", releasing)
        self.assertNotIn("SHA-256 of each of the two assets", releasing)
        self.assertNotIn("SHA-256 table", releasing)
        self.assertIn("Quest APKs are published outside GitHub", releasing)
        self.assertNotIn("echo.taxi", releasing)
        self.assertIn("The zip has no `plugins/` entry", releasing)
        self.assertNotIn("an empty `plugins/`", releasing)


if __name__ == "__main__":
    unittest.main()
