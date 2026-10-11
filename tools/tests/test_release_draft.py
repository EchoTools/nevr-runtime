"""tools/release_draft.sh: which kind of release a tag has, for a build started by hand on that tag's ref.

GitHub's "get a release by tag name" returns published releases only, so the draft is found by listing.
The fake gh applies the script's own --jq expression to a canned release list with the real jq."""

import json
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release_draft.sh"

FAKE_GH = """#!/usr/bin/env bash
# Fake gh: `api --paginate <path> --jq <expr>` runs <expr> over $FAKE_RELEASES (a JSON array) with jq, or
# fails with $FAKE_API_RC.
set -u
if [ "$1" = api ]; then
  [ "${FAKE_API_RC:-0}" -eq 0 ] || { echo "gh: HTTP 403: Resource not accessible by integration" >&2; exit "${FAKE_API_RC}"; }
  shift
  expr=""
  while [ $# -gt 0 ]; do
    case "$1" in --jq) expr=$2; shift;; esac
    shift
  done
  printf '%s' "$FAKE_RELEASES" | jq -r "$expr"
  exit 0
fi
echo "unexpected gh call: $*" >&2
exit 99
"""

TAG = "v5.0.0"


def release(tag, draft, assets=()):
    return {"tag_name": tag, "draft": draft, "id": 1, "assets": [{"name": name} for name in assets]}


FIVE = ["nevr-runtime-v5.0.0-windows.zip", "SHA256SUMS", "RELEASE-NOTES.md", "nevr-runtime-v5.0.0.zip",
        "nevr-runtime-v5.0.0-lite.zip"]


class ReleaseDraftTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="release-draft-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        bindir = self.tmp / "bin"
        bindir.mkdir()
        gh = bindir / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}")

    def run_script(self, releases, tag=TAG, **env):
        return subprocess.run([str(SCRIPT), tag, "EchoTools/nevr-runtime"],
                              env=dict(self.env, FAKE_RELEASES=json.dumps(releases), **env),
                              capture_output=True, text=True)

    def test_exactly_one_draft_of_the_tag_is_a_draft(self):
        result = self.run_script([release(TAG, True), release("v4.9.0", False), release("v5.0.1", True)])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "draft"), result.stderr)

    def test_a_draft_that_already_carries_the_built_files_is_refused_never_overwritten(self):
        # A second dispatch after the signer replaced the draft's assets would rebuild UNSIGNED files over
        # the signed ones: it fails red and uploads nothing.
        result = self.run_script([release(TAG, True, FIVE)])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotEqual(result.stdout.strip(), "draft")
        self.assertIn("draft v5.0.0 already carries built assets; refusing to overwrite", result.stderr)

    def test_a_draft_with_some_but_not_all_of_the_files_needs_a_human(self):
        for name in FIVE:
            result = self.run_script([release(TAG, True, [name])])
            self.assertEqual(result.returncode, 1, name)
            self.assertIn("refusing to overwrite", result.stderr)

    def test_a_draft_with_only_unrelated_assets_proceeds(self):
        # An APK or notes the human attached by hand are not the build's files.
        result = self.run_script([release(TAG, True, ["nevr-runtime-v5.0.0-quest.apk", "notes.txt"])])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "draft"), result.stderr)

    def test_another_tags_assets_do_not_count(self):
        result = self.run_script([release(TAG, True), release("v4.9.0", False, FIVE)])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "draft"), result.stderr)

    def test_no_release_of_the_tag_is_none_a_dry_run(self):
        result = self.run_script([release("v4.9.0", False), release("v5.0.1", True)])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "none"), result.stderr)
        self.assertIn("no release", result.stderr)

    def test_a_published_release_of_the_tag_is_none_and_never_a_draft(self):
        result = self.run_script([release(TAG, False)])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "none"), result.stderr)
        self.assertIn("published release", result.stderr)

    def test_a_draft_beside_a_published_release_of_the_tag_is_refused(self):
        result = self.run_script([release(TAG, True), release(TAG, False)])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotEqual(result.stdout.strip(), "draft")

    def test_two_drafts_of_the_tag_are_refused(self):
        result = self.run_script([release(TAG, True), release(TAG, True)])
        self.assertEqual(result.returncode, 1)
        self.assertIn("2 draft releases", result.stderr)

    def test_a_failed_listing_fails_it_does_not_mean_none(self):
        result = self.run_script([], FAKE_API_RC="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HTTP 403", result.stderr)
        self.assertNotIn("none", result.stdout)

    def test_a_tag_that_is_not_vx_y_z_is_refused_before_any_call(self):
        for tag in ("v5.0.0-rc.1", "v5.0", "main", 'v5.0.0"] | .x'):
            result = self.run_script([release(TAG, True)], tag=tag, FAKE_API_RC="7")
            self.assertEqual(result.returncode, 2, tag)
            self.assertIn("not a vX.Y.Z tag", result.stderr)


if __name__ == "__main__":
    unittest.main()
