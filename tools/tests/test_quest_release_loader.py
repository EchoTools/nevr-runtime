"""#1135: the loader in a release APK is the release's own asset.

tools/quest_release_loader.py against a fake `gh`: `release view` prints the release's assets as JSON, `release
download` copies the asset into --dir, `attestation verify` exits as the test says. The fake records every call.
"""

import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/quest_release_loader.py"
NAME = "nevr-runtime-v5.16.2-quest-libovrplatformloader.so"
BYTES = b"\x7fELF-the-ci-built-loader"
SHA = hashlib.sha256(BYTES).hexdigest()

FAKE_GH = r'''#!/usr/bin/env python3
import json, os, pathlib, shutil, sys
d = pathlib.Path(os.environ["FAKE_GH_DIR"])
with (d / "calls.log").open("a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
a = sys.argv[1:]
if a[:2] == ["release", "view"]:
    print((d / "assets.json").read_text())
elif a[:2] == ["release", "download"]:
    out = pathlib.Path(a[a.index("--dir") + 1])
    shutil.copy(d / "asset.bin", out / a[a.index("--pattern") + 1])
elif a[:2] == ["attestation", "verify"]:
    sys.exit(int(os.environ.get("FAKE_GH_ATTEST", "0")))
else:
    sys.exit(64)
'''


class QuestReleaseLoaderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="quest-release-loader-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.fake = self.tmp / "fake"
        self.fake.mkdir()
        gh = self.tmp / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(gh.stat().st_mode | stat.S_IXUSR)
        self.out = self.tmp / "out"
        self.env = dict(os.environ, GH_BIN=str(gh), FAKE_GH_DIR=str(self.fake))
        self.release(assets=[{"name": NAME, "size": len(BYTES), "digest": f"sha256:{SHA}"}])

    def release(self, assets, asset_bytes=BYTES):
        (self.fake / "assets.json").write_text(json.dumps({"assets": assets}))
        (self.fake / "asset.bin").write_bytes(asset_bytes)

    def run_tool(self, *args, **env):
        return subprocess.run([sys.executable, "-I", str(TOOL), *args], capture_output=True, text=True,
                              env=dict(self.env, **env), timeout=60)

    def calls(self):
        log = self.fake / "calls.log"
        return log.read_text().splitlines() if log.exists() else []

    def fetch(self, tag="v5.16.2", **env):
        return self.run_tool("fetch", "--tag", tag, "--out", str(self.out), **env)

    def refused(self, done):
        self.assertEqual((done.returncode, done.stdout), (1, ""), done.stderr)
        record = json.loads(done.stderr.strip().splitlines()[-1])
        self.assertEqual((record["tag"], record["event"]), ("NEVR.QUEST.LOADER", "refused"))
        return record["reason"]

    def test_it_fetches_the_asset_checks_it_and_prints_its_sha256(self):
        done = self.fetch()
        self.assertEqual((done.returncode, done.stdout), (0, SHA + "\n"), done.stderr)
        self.assertEqual((self.out / "libovrplatformloader.so").read_bytes(), BYTES)
        self.assertEqual([p.name for p in self.out.iterdir()], ["libovrplatformloader.so"])
        calls = self.calls()
        self.assertTrue(any(c.startswith("release view v5.16.2 --repo EchoTools/nevr-runtime") for c in calls), calls)
        self.assertTrue(any(f"--pattern {NAME}" in c for c in calls), calls)
        self.assertTrue(any(c.startswith("attestation verify ") and c.endswith(" -R EchoTools/nevr-runtime")
                            for c in calls), calls)

    def test_a_tag_that_is_not_a_release_tag_is_refused_before_gh_runs(self):
        for tag in ("5.16.2", "v5.16", "v05.16.2", "v5.16.2-dev.1", "main"):
            with self.subTest(tag=tag):
                self.assertIn("not a release tag", self.refused(self.fetch(tag)))
        self.assertEqual(self.calls(), [])

    def test_a_release_without_the_asset_is_refused(self):
        self.release(assets=[{"name": "nevr-runtime-v5.16.2-windows.zip", "size": 1, "digest": "sha256:" + "0" * 64}])
        self.assertIn("0 asset(s)", self.refused(self.fetch()))

    def test_two_assets_of_the_name_are_refused(self):
        asset = {"name": NAME, "size": len(BYTES), "digest": f"sha256:{SHA}"}
        self.release(assets=[asset, asset])
        self.assertIn("2 asset(s)", self.refused(self.fetch()))

    def test_an_asset_with_no_recorded_digest_is_refused(self):
        self.release(assets=[{"name": NAME, "size": len(BYTES)}])
        self.assertIn("no sha256 digest", self.refused(self.fetch()))
        self.assertFalse(any(c.startswith("release download") for c in self.calls()))

    def test_bytes_that_differ_from_the_recorded_digest_are_refused_and_left_nowhere(self):
        tampered = b"\x7fELF-another-build!!!!!!"
        self.assertEqual(len(tampered), len(BYTES))
        self.release(assets=[{"name": NAME, "size": len(BYTES), "digest": f"sha256:{SHA}"}], asset_bytes=tampered)
        self.assertIn("recorded", self.refused(self.fetch()))
        self.assertEqual(list(self.out.rglob("*.so")), [])
        self.assertFalse(any(c.startswith("attestation") for c in self.calls()))

    def test_a_size_that_differs_from_the_release_is_refused(self):
        self.release(assets=[{"name": NAME, "size": len(BYTES) + 1, "digest": f"sha256:{SHA}"}])
        self.assertIn("bytes, the release says", self.refused(self.fetch()))

    def test_a_failed_attestation_is_refused_and_leaves_no_loader(self):
        done = self.fetch(FAKE_GH_ATTEST="1")
        self.assertIn("attestation", self.refused(done))
        self.assertEqual(list(self.out.rglob("*.so")), [])

    def test_a_gh_that_cannot_run_is_refused(self):
        self.assertIn("cannot run", self.refused(self.fetch(GH_BIN=str(self.tmp / "no-such-gh"))))


class VerifyApkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="quest-release-loader-apk-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.apk = self.tmp / "q.apk"
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", BYTES)
            z.writestr("lib/arm64-v8a/libovrplatformloader_orig.so", b"the store's loader")

    def run_tool(self, apk, sha):
        return subprocess.run([sys.executable, "-I", str(TOOL), "verify-apk", "--apk", str(apk), "--sha256", sha],
                              capture_output=True, text=True, timeout=60)

    def test_the_loader_with_the_assets_sha256_passes(self):
        done = self.run_tool(self.apk, SHA)
        self.assertEqual((done.returncode, done.stdout), (0, ""), done.stderr)
        self.assertEqual(json.loads(done.stderr)["event"], "apk_loader_matches_asset")

    def test_a_different_loader_is_refused_naming_both_digests(self):
        done = self.run_tool(self.apk, "0" * 64)
        self.assertEqual(done.returncode, 1)
        reason = json.loads(done.stderr)["reason"]
        self.assertIn(SHA, reason)
        self.assertIn("0" * 64, reason)

    def test_an_apk_with_no_loader_is_refused(self):
        bare = self.tmp / "bare.apk"
        with zipfile.ZipFile(bare, "w") as z:
            z.writestr("AndroidManifest.xml", b"x")
        self.assertEqual(self.run_tool(bare, SHA).returncode, 1)

    def test_a_sha256_that_is_not_hex_is_refused(self):
        self.assertEqual(self.run_tool(self.apk, "NOT-HEX").returncode, 1)


if __name__ == "__main__":
    unittest.main()
