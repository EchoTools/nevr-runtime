"""#758: only a development build may download game data, so the Quest sentinel's `IsDevelopment()` must be false for a
release (#1935: the flag is `!IsRelease()` of the one build stamp unit, src/core/build_stamp.h).

A release is a CI build exactly on the tag vX.Y.Z of the committed VERSION file, at the commit being built, with no
uncommitted change (cmake/nevr_build_stamp.cmake refuses a dirty release). Every other build is a development build,
including a build of a release-candidate tag: a tag name that is not the VERSION file's release never makes a release.

`src/quest/sentinel/entry.cpp` sets `options.allow_fetch = nevr_build_stamp::IsDevelopment()`. This test runs the real
stamp script in a scratch git repository, under the CI variables of each case, and reads the generated unit's kRelease.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAMP = ROOT / "cmake" / "nevr_build_stamp.cmake"
GIT_ENV = {
    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
}


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "needs cmake and git")
class IsDevelopmentFollowsTheRelease(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        (self.repo / "VERSION").write_text("5.5.0\n")
        (self.repo / "tracked.txt").write_text("one\n")
        self.git("init", "-q", "-b", "main")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "one")
        self.git("tag", "-a", "v5.4.0", "-m", "x")
        self.git("commit", "-q", "--allow-empty", "-m", "two")
        self.out = self.repo / "generated" / "nevr_build_stamp.cpp"

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *cmd):
        return subprocess.run(["git", "-c", "tag.gpgSign=false", "-c", "commit.gpgSign=false", *cmd], cwd=self.repo,
                              env={**os.environ, **GIT_ENV}, check=True, capture_output=True, text=True)

    def stamp(self, **ci):
        """(returncode, kRelease or None, output) of one run of the real stamp script under the given CI variables."""
        env = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        env.update(ci)
        shutil.rmtree(self.out.parent, ignore_errors=True)
        result = subprocess.run(["cmake", f"-DNEVR_STAMP_SOURCE_DIR={self.repo}", f"-DNEVR_STAMP_OUTPUT={self.out}",
                                 "-P", str(STAMP)], env=env, capture_output=True, text=True)
        text = " ".join((result.stdout + result.stderr).split())
        if result.returncode != 0 or not self.out.exists():
            return result.returncode, None, text
        flag = "const bool kRelease = true;" in self.out.read_text()
        return result.returncode, flag, text

    def is_development(self, **ci):
        """IsDevelopment() of the build: the negation of kRelease (src/core/build_stamp.h)."""
        rc, release, text = self.stamp(**ci)
        self.assertEqual(rc, 0, text)
        return not release

    def tag_build(self, name):
        return dict(GITHUB_ACTIONS="true", GITHUB_REF_TYPE="tag", GITHUB_REF_NAME=name)

    def test_a_ci_build_exactly_on_the_version_tag_is_not_a_development_build(self):
        self.git("tag", "-a", "v5.5.0", "-m", "x")
        self.assertFalse(self.is_development(**self.tag_build("v5.5.0")))

    def test_any_other_build_is_a_development_build(self):
        self.assertTrue(self.is_development())

    def test_a_tag_ref_of_another_tag_name_is_a_development_build(self):
        self.git("tag", "-a", "v5.5.0", "-m", "x")
        self.assertTrue(self.is_development(**self.tag_build("v9.9.9")))

    def test_a_ci_build_outside_actions_or_on_a_branch_ref_is_a_development_build(self):
        self.git("tag", "-a", "v5.5.0", "-m", "x")
        self.assertTrue(self.is_development(GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v5.5.0"))
        self.assertTrue(self.is_development(GITHUB_ACTIONS="true", GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="v5.5.0"))
        self.assertTrue(self.is_development(GITHUB_ACTIONS="false", GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v5.5.0"))

    def test_a_release_candidate_tag_is_never_a_release(self):
        self.git("tag", "-a", "v5.5.0-rc.1", "-m", "x")
        self.assertTrue(self.is_development(**self.tag_build("v5.5.0-rc.1")))
        self.git("tag", "-a", "v5.5.0", "-m", "x")
        self.assertTrue(self.is_development(**self.tag_build("v5.5.0-rc.1")))

    def test_a_tag_ref_that_does_not_point_at_the_built_commit_is_a_development_build(self):
        self.git("tag", "-a", "v5.5.0", "-m", "x")
        self.git("commit", "-q", "--allow-empty", "-m", "later")
        self.assertTrue(self.is_development(**self.tag_build("v5.5.0")))

    def test_a_release_on_a_dirty_tree_refuses_to_build(self):
        self.git("tag", "-a", "v5.5.0", "-m", "x")
        (self.repo / "tracked.txt").write_text("edited\n")
        rc, release, text = self.stamp(**self.tag_build("v5.5.0"))
        self.assertNotEqual(rc, 0, text)
        self.assertIsNone(release)
        self.assertIn("-dirty", text)


if __name__ == "__main__":
    unittest.main()
