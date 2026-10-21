"""tools/release_draft.sh: which kind of release a tag has, for a build started by hand on that tag's ref.

The releases of the tag arrive as JSON on stdin (the `drafts` job of build.yml lists them with the write
token; this script holds no token and makes no call), so the tests feed it the listing directly and a `gh`
that fails the test if it is ever run stands on PATH."""

import json
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "release_draft.sh"

NO_GH = """#!/usr/bin/env bash
echo "release_draft.sh called gh: $*" >&2
exit 99
"""

TAG = "v5.0.0"


def release(draft, assets=()):
    return {"draft": draft, "assets": list(assets)}


FOUR = ["nevr-runtime-v5.0.0-windows.zip", "SHA256SUMS", "RELEASE-NOTES.md", "nevr-runtime-v5.0.0.zip",
        "nevr-runtime-v5.0.0-quest-libovrplatformloader.so"]  # five names; the name FOUR predates the Quest library


class ReleaseDraftTest(unittest.TestCase):
    def setUp(self):
        # release_draft.sh and the `drafts` step read JSON with jq, which the builder image does not carry; they
        # run on the ubuntu-26.04 runner, which does. A skip here names why, never a pass.
        if shutil.which("jq") is None:
            self.skipTest("jq is not on PATH (the CI builder image has none; the script runs on the ubuntu-26.04 runner)")
        self.tmp = Path(tempfile.mkdtemp(prefix="release-draft-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        bindir = self.tmp / "bin"
        bindir.mkdir()
        gh = bindir / "gh"
        gh.write_text(NO_GH)
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
        self.env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}")

    def run_script(self, releases, tag=TAG, raw=None):
        stdin = raw if raw is not None else json.dumps(releases)
        return subprocess.run([str(SCRIPT), tag], input=stdin, env=self.env, capture_output=True, text=True)

    def test_an_empty_or_failing_name_list_is_an_error_never_a_draft(self):
        # the names file is sourced from the script's own directory: run a copy whose list is broken
        import shutil
        for n, body in enumerate(("release_asset_names() { return 1; }\n", "release_asset_names() { :; }\n")):
            with self.subTest(body=body):
                d = self.tmp / f"copy-{n}"
                d.mkdir()
                for f in ("release_draft.sh", "release_asset_names.sh"):
                    shutil.copy(REPO / "tools" / f, d / f)
                with open(d / "release_asset_names.sh", "a") as fh:
                    fh.write(body)
                result = subprocess.run([str(d / "release_draft.sh"), TAG], input=json.dumps([release(True, FOUR)]),
                                        env=self.env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertNotEqual(result.stdout.strip(), "draft")

    def test_exactly_one_draft_of_the_tag_is_a_draft(self):
        result = self.run_script([release(True)])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "draft"), result.stderr)

    def test_a_draft_that_already_carries_the_built_files_is_refused_never_overwritten(self):
        # A second dispatch after the signer replaced the draft's assets would rebuild UNSIGNED files over
        # the signed ones: it fails red and uploads nothing.
        result = self.run_script([release(True, FOUR)])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotEqual(result.stdout.strip(), "draft")
        self.assertIn("the draft already carries built assets", result.stderr)
        self.assertIn("refusing to overwrite", result.stderr)

    def test_a_draft_with_some_but_not_all_of_the_files_needs_a_human(self):
        for name in FOUR:
            result = self.run_script([release(True, [name])])
            self.assertEqual(result.returncode, 1, name)
            self.assertIn("refusing to overwrite", result.stderr)

    def test_a_draft_with_only_unrelated_assets_proceeds(self):
        # An APK or notes the human attached by hand are not the build's files.
        result = self.run_script([release(True, ["nevr-runtime-v5.0.0-quest.apk", "notes.txt"])])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "draft"), result.stderr)

    def test_the_published_releases_assets_do_not_count_on_the_draft(self):
        # a published release beside the draft is refused for being ambiguous, never for its assets
        result = self.run_script([release(True), release(False, FOUR)])
        self.assertEqual(result.returncode, 1)
        self.assertIn("both a draft and a published release", result.stderr)

    def test_no_release_of_the_tag_is_none_a_dry_run(self):
        result = self.run_script([])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "none"), result.stderr)
        self.assertIn("0 draft(s), 0 published", result.stderr)

    def test_a_published_release_of_the_tag_is_none_and_never_a_draft(self):
        result = self.run_script([release(False)])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "none"), result.stderr)
        self.assertIn("0 draft(s), 1 published", result.stderr)

    def test_a_draft_beside_a_published_release_of_the_tag_is_refused(self):
        result = self.run_script([release(True), release(False)])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotEqual(result.stdout.strip(), "draft")

    def test_two_drafts_of_the_tag_are_refused(self):
        result = self.run_script([release(True), release(True)])
        self.assertEqual(result.returncode, 1)
        self.assertIn("2 draft releases", result.stderr)
        self.assertIn("2 draft(s), 0 published", result.stderr)

    def test_a_listing_that_is_not_a_json_array_of_releases_is_never_read_as_none(self):
        for raw in ("", "null", "{}", "not json", '[{"assets": []}]', '[{"draft": "yes"}]'):
            result = self.run_script(None, raw=raw)
            self.assertEqual(result.returncode, 1, repr(raw))
            self.assertNotIn("none", result.stdout)
            self.assertIn("refusing to decide", result.stderr)

    def test_the_decision_line_names_the_counts_and_the_outcome(self):
        cases = [([release(True)], "1 draft(s), 0 published, 0 of 5 built names on the draft(s): draft"),
                 ([release(True, FOUR[:2])], "1 draft(s), 0 published, 2 of 5 built names on the draft(s): refused"),
                 ([], "0 draft(s), 0 published, 0 of 5 built names on the draft(s): none")]
        for releases, line in cases:
            self.assertIn(line, self.run_script(releases).stderr)

    def test_a_tag_that_is_not_vx_y_z_is_refused_before_any_call(self):
        for tag in ("v5.0.0-rc.1", "v5.0", "main", 'v5.0.0"] | .x'):
            result = self.run_script([release(True)], tag=tag)
            self.assertEqual(result.returncode, 2, tag)
            self.assertIn("not a vX.Y.Z tag", result.stderr)


if __name__ == "__main__":
    unittest.main()
