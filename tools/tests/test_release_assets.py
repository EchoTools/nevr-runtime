"""One release asset per audience, named for it (#823): the list, the build, and RELEASING.md agree."""
import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
NAMES = REPO / "tools" / "release_asset_names.sh"
AUDIENCE = ("windows-player", "windows-server", "quest")


def listed(version="5.5.0"):
    out = subprocess.run(["bash", "-c", f'source "{NAMES}"; release_asset_names {version}'],
                         capture_output=True, text=True, check=True).stdout
    return out.split("\n")[:-1]


class ReleaseAssetsTest(unittest.TestCase):
    def test_the_release_lists_exactly_these_five_names(self):
        self.assertEqual(listed(), [
            "nevr-runtime-v5.5.0-windows-player.zip", "SHA256SUMS", "RELEASE-NOTES.md",
            "nevr-runtime-v5.5.0-windows-server.zip", "nevr-runtime-v5.5.0-quest-libovrplatformloader.so"])

    def test_every_per_audience_file_carries_its_audience_and_the_bare_zip_is_gone(self):
        for name in listed():
            if name in ("SHA256SUMS", "RELEASE-NOTES.md"):
                continue
            self.assertTrue(any(f"-{a}" in name for a in AUDIENCE), f"{name} names no audience")
        self.assertNotIn("nevr-runtime-v5.5.0.zip", listed())

    def test_the_packager_and_the_dist_target_make_the_listed_zips(self):
        packager = (REPO / "tools" / "package_release.py").read_text()
        self.assertIn('f"{base}-windows-player.zip"', packager)
        self.assertNotIn('-windows.zip', packager)
        cmake = (REPO / "CMakeLists.txt").read_text()
        self.assertIn('set(DIST_NAME "nevr-runtime-v${PROJECT_VERSION}-windows-server")', cmake)

    def test_the_workflow_globs_name_the_player_tree(self):
        build = (REPO / ".github/workflows/build.yml").read_text()
        self.assertEqual(len(re.findall(r"nevr-runtime-v\*-windows-player", build)), 2)
        self.assertIsNone(re.search(r"nevr-runtime-v\*-windows(?!-)", build))

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

    def test_releasing_md_covers_the_failed_draft_and_both_checksum_files_after_signing(self):
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        self.assertIn("A draft that fails its checks is deleted and cut again", releasing)
        self.assertIn("gh release delete vX.Y.Z", releasing)
        self.assertIn("The signer rewrites **both** checksum files", releasing)
        self.assertIn("covers the unsigned build output CI attested", releasing)

    def test_readme_issue_template_and_justfile_carry_the_audience_names_and_sums_scope(self):
        self.assertIn("-windows-player.zip", (REPO / "README.md").read_text(encoding="utf-8"))
        self.assertIn("-windows-player.zip", (REPO / ".github/ISSUE_TEMPLATE/beta-bug.yml").read_text(encoding="utf-8"))
        self.assertIn("-windows-server) and its symbol files", (REPO / "justfile").read_text(encoding="utf-8"))
        releasing = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        self.assertIn("the SHA-256 of the Windows player zip", releasing)
        self.assertIn("Quest APKs and dry runs are published outside GitHub", releasing)
        self.assertNotIn("echo.taxi", releasing)
        self.assertIn("the zip has no `plugins/` entry", releasing)
        self.assertNotIn("an empty `plugins/`", releasing)


if __name__ == "__main__":
    unittest.main()
