"""#989, #1135: a release's Quest APK has a recipe, and the loader in it is the release's own asset.

`just release-quest-apk vX.Y.Z` repacks the store game with the loader CI built for the tag and attached to the
release (tools/quest_release_loader.py), instead of compiling a second one: the asset and the APK's loader are one
file, and the recipe refuses unless their sha256 agree. This test reads the justfile text (the recipe cannot run
here: it needs the signing key, the store APK and a release).
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

    def test_it_compiles_nothing(self):
        body = recipe("release-quest-apk")
        for forbidden in ("_quest-apk", "cmake", "GITHUB_ACTIONS", "build-android", "android-dev"):
            self.assertNotIn(forbidden, body)

    def test_it_packages_the_release_asset_and_refuses_unless_the_apk_carries_it(self):
        body = recipe("release-quest-apk")
        fetch = body.index("tools/quest_release_loader.py fetch --tag")
        identity = body.index("tools/package_release.py quest-library")
        repack = body.index("just android-repack-apk")
        verify = body.index("tools/quest_release_loader.py verify-apk")
        copy = body.index('cp -f "$out/repack/r15_nevr-sentinel_signed.apk"')
        self.assertLess(fetch, identity)
        self.assertLess(identity, repack)
        self.assertLess(repack, verify)
        self.assertLess(verify, copy)
        self.assertIn('--version "$ver" --commit "$tag_commit"', body)
        self.assertIn('"$out/loader/libovrplatformloader.so"', body[repack:verify])
        self.assertIn('"$ver"', body[repack:verify])  # the versionCode comes from the tag, not from a build tree
        self.assertIn('--sha256 "$loader_sha"', body[verify:copy])

    def test_the_shared_recipe_builds_a_development_package_only(self):
        body = recipe("_quest-apk")
        self.assertNotIn("--fresh", body)
        self.assertNotIn("mode", body.splitlines()[0])

    def test_the_repack_takes_a_version_where_there_is_no_build_info(self):
        body = recipe("android-repack-apk")
        self.assertIn('version=""', body.splitlines()[0])
        self.assertIn('--version "$version"', body)

    def test_package_dev_and_the_release_share_one_quest_build(self):
        self.assertIn('just _quest-apk "{{ store_apk }}"', recipe("package-dev"))
        self.assertNotIn("just android-repack-apk", recipe("package-dev"))


if __name__ == "__main__":
    unittest.main()
