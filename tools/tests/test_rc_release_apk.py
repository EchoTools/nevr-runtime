"""tools/rc_release_apk.sh: a release candidate's APK is listed, then downloaded; no failure is swallowed."""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "rc_release_apk.sh"

FAKE_GH = """#!/usr/bin/env bash
# Fake gh: `release view` prints $FAKE_ASSETS (or fails with $FAKE_VIEW_RC); `release download` writes the
# pattern's file into --dir unless $FAKE_DOWNLOAD_RC is nonzero.
set -u
if [ "$1 $2" = "release view" ]; then
  [ "${FAKE_VIEW_RC:-0}" -eq 0 ] || { echo "gh: HTTP 404: Not Found" >&2; exit "${FAKE_VIEW_RC}"; }
  printf '%s' "$FAKE_ASSETS"
  exit 0
fi
if [ "$1 $2" = "release download" ]; then
  [ "${FAKE_DOWNLOAD_RC:-0}" -eq 0 ] || { echo "gh: download failed" >&2; exit "${FAKE_DOWNLOAD_RC}"; }
  while [ $# -gt 0 ]; do
    case "$1" in --pattern) pattern=$2; shift;; --dir) dir=$2; shift;; esac
    shift
  done
  mkdir -p "$dir" && printf 'APK' > "$dir/$pattern"
  exit 0
fi
echo "unexpected gh call: $*" >&2
exit 99
"""


class RcReleaseApkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="rc-apk-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        bindir = self.tmp / "bin"
        bindir.mkdir()
        gh = bindir / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}")

    def run_script(self, **env):
        return subprocess.run([str(SCRIPT), "v4.0.0-rc.1", "EchoTools/nevr-runtime", str(self.tmp / "apk")],
                              env=dict(self.env, **env), capture_output=True, text=True)

    def test_a_listed_apk_is_downloaded_and_its_path_printed(self):
        result = self.run_script(FAKE_ASSETS="nevr-runtime-v4.0.0-rc.1-windows.zip\nnevr-runtime-v4.0.0-rc.1-quest.apk\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(self.tmp / "apk" / "nevr-runtime-v4.0.0-rc.1-quest.apk"))
        self.assertEqual(Path(result.stdout.strip()).read_bytes(), b"APK")

    def test_a_release_that_lists_no_apk_is_a_zip_only_candidate(self):
        result = self.run_script(FAKE_ASSETS="nevr-runtime-v4.0.0-rc.1-windows.zip\nSHA256SUMS\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("a zip-only release candidate", result.stderr)
        self.assertFalse((self.tmp / "apk").exists())

    def test_a_failed_listing_fails_it_does_not_mean_no_apk(self):
        result = self.run_script(FAKE_ASSETS="", FAKE_VIEW_RC="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HTTP 404", result.stderr)
        self.assertNotIn("covers the Windows zip only", result.stdout + result.stderr)

    def test_a_listed_apk_that_cannot_be_downloaded_fails(self):
        result = self.run_script(FAKE_ASSETS="a-quest.apk\n", FAKE_DOWNLOAD_RC="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("download failed", result.stderr)

    def test_two_apks_are_refused(self):
        result = self.run_script(FAKE_ASSETS="a-quest.apk\nb-quest.apk\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected one", result.stderr)


if __name__ == "__main__":
    unittest.main()
