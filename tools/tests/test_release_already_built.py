"""tools/release_already_built.sh: does a release already carry the build's whole asset set?

The signal is the release's asset NAMES (five files for version X.Y.Z), never the pre-release flag.
A listing that fails is an error, not "false"."""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release_already_built.sh"

FAKE_GH = """#!/usr/bin/env bash
set -u
echo "$*" >> "$FAKE_GH_LOG"
[ "$1 $2" = "release view" ] || exit 99
[ "${FAKE_VIEW_RC:-0}" -eq 0 ] || { echo "gh: HTTP 404: Not Found" >&2; exit "${FAKE_VIEW_RC}"; }
printf '%s' "$FAKE_ASSETS"
"""


class ReleaseAlreadyBuiltTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="already-built-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        bindir = self.tmp / "bin"
        bindir.mkdir()
        gh = bindir / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}", FAKE_GH_LOG=str(self.tmp / "log"))

    def run_script(self, tag="v5.0.0", **env):
        return subprocess.run([str(SCRIPT), tag, "EchoTools/nevr-runtime"], env=dict(self.env, **env),
                              capture_output=True, text=True)

    def names(self, version="5.0.0", drop=None):
        names = [f"nevr-runtime-v{version}-windows.zip", "SHA256SUMS", "RELEASE-NOTES.md",
                 f"nevr-runtime-v{version}.zip", f"nevr-runtime-v{version}-lite.zip"]
        return "\n".join(n for n in names if n != drop) + "\n"

    def test_all_five_assets_mean_already_built(self):
        result = self.run_script(FAKE_ASSETS=self.names())
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "true"), result.stderr)

    def test_one_missing_asset_means_not_built_and_names_it(self):
        result = self.run_script(FAKE_ASSETS=self.names(drop="RELEASE-NOTES.md"))
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "false"))
        self.assertIn("RELEASE-NOTES.md", result.stderr)

    def test_every_one_of_the_five_is_required(self):
        every = self.names().split()
        self.assertEqual(len(every), 5)
        for name in every:
            with self.subTest(missing=name):
                result = self.run_script(FAKE_ASSETS=self.names(drop=name))
                self.assertEqual(result.stdout.strip(), "false")
                self.assertIn(name, result.stderr)

    def test_the_tar_zst_archives_are_not_required_and_do_not_count(self):
        result = self.run_script(FAKE_ASSETS=self.names() + "nevr-runtime-v5.0.0.tar.zst\n")
        self.assertEqual(result.stdout.strip(), "true")
        only_tars = "nevr-runtime-v5.0.0.tar.zst\nnevr-runtime-v5.0.0-lite.tar.zst\nSHA256SUMS\nRELEASE-NOTES.md\n"
        self.assertEqual(self.run_script(FAKE_ASSETS=only_tars).stdout.strip(), "false")

    def test_extra_assets_do_not_matter(self):
        result = self.run_script(FAKE_ASSETS=self.names() + "something-else.txt\n")
        self.assertEqual(result.stdout.strip(), "true")

    def test_a_partial_name_is_not_a_match(self):
        names = self.names().replace("SHA256SUMS", "SHA256SUMS.sig")
        self.assertEqual(self.run_script(FAKE_ASSETS=names).stdout.strip(), "false")

    def test_a_failed_listing_is_an_error_not_false(self):
        result = self.run_script(FAKE_ASSETS="", FAKE_VIEW_RC="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("false", result.stdout)

    def test_a_tag_that_is_not_vx_y_z_is_an_error_and_gh_is_not_called(self):
        result = self.run_script(tag="v5.0.0-beta.1", FAKE_ASSETS=self.names())
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.tmp / "log").exists())

    def test_the_prerelease_flag_is_never_asked_for(self):
        self.run_script(FAKE_ASSETS=self.names())
        call = (self.tmp / "log").read_text()
        self.assertIn("--json assets", call)
        self.assertNotIn("prerelease", call)


if __name__ == "__main__":
    unittest.main()
