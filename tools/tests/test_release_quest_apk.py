"""#989: a release's Quest APK has a recipe, and it builds the tag's identity, not a development build.

`just release-quest-apk vX.Y.Z` repacks the store game from the release tag. Its build must be a CI tag build (the
environment `cmake/nevr_build_info.cmake` reads) reconfigured with --fresh, and it must check the generated header
for the tag's version and kIsDev = false before it copies the APK. This test reads the justfile text (the recipe
cannot run here: it needs the signing key, the store APK and the NDK).
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
JUSTFILE = (ROOT / "justfile").read_text()


def recipe(name):
    m = re.search(rf"^{re.escape(name)}\b[^\n]*:\n((?:[ \t]+[^\n]*\n|\n)*)", JUSTFILE, re.M)
    if not m:
        raise AssertionError(f"justfile has no recipe {name}")
    return m.group(0)


class ReleaseQuestApk(unittest.TestCase):
    def test_the_release_recipe_exists_and_refuses_a_non_release_tag(self):
        body = recipe("release-quest-apk")
        self.assertIn("release-quest-apk tag", body.splitlines()[0])
        self.assertIn(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$", body)

    def test_it_builds_with_the_tag_ci_identity_through_the_shared_quest_recipe(self):
        body = recipe("release-quest-apk")
        self.assertIn("GITHUB_ACTIONS=true GITHUB_REF_TYPE=tag GITHUB_REF_NAME=\"$tag\" just _quest-apk", body)
        self.assertIn("_quest-apk \"{{ store_apk }}\" release", body)

    def test_the_shared_recipe_reconfigures_fresh_only_for_a_release(self):
        body = recipe("_quest-apk")
        self.assertIn('{{ if mode == "release" { "--fresh" } else { "" } }}', body)

    def test_it_checks_the_header_for_the_tag_version_and_a_non_dev_build_before_copying(self):
        body = recipe("release-quest-apk")
        check = body.index('kVersion = \\"$ver\\"')
        dev = body.index("kIsDev = false;")
        copy = body.index('cp -f build/android-dev/repack/r15_nevr-sentinel_signed.apk')
        self.assertLess(max(check, dev), copy)

    def test_package_dev_and_the_release_share_one_quest_build(self):
        self.assertIn('just _quest-apk "{{ store_apk }}"', recipe("package-dev"))
        self.assertNotIn("just android-repack-apk", recipe("package-dev"))


if __name__ == "__main__":
    unittest.main()
