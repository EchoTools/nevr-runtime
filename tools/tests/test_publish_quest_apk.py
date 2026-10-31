"""`tools/publish_quest_apk.py` (#1098): the environment-only refusals, the upload through the signer's `put-quest-apk`
and `rclone`, and the fetch-back from the public host. The uploader and `rclone` are fakes that record what they were
called with; the public host is a server on 127.0.0.1 that serves a directory the test controls."""

import hashlib
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "publish_quest_apk.py"

TAG = "v1.2.3"
APK_NAME = f"nevr-runtime-{TAG}-quest.apk"
APK_BYTES = b"PK\x03\x04 this stands in for an APK " * 64
VARIANT = "q15t"
VARIANT_NAME = f"nevr-runtime-{TAG}-{VARIANT}-quest.apk"
VARIANT_BYTES = b"PK\x03\x04 this stands in for the variant APK " * 48
SECRETS = {
    "R2_ENDPOINT": "https://sentinel-endpoint.invalid",
    "R2_BUCKET": "sentinel-bucket",
    "R2_ACCESS_KEY_ID": "sentinel-access-key-id",
    "R2_SECRET_ACCESS_KEY": "sentinel-secret-access-key",
    "NEVR_RCLONE_REMOTE": "sentinel_remote",
    "GH_TOKEN": "sentinel-gh-token",
}
REQUIRED = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "RELEASE_PUBLIC_BASE",
            "RELEASE_SIGNER_BIN", "RELEASE_SIGNER_DIR", "GH_TOKEN", "NEVR_RCLONE_REMOTE")

# Records its arguments and whether the R2 credentials reached it, then exits with $FAKE_SIGNER_EXIT.
FAKE_SIGNER = """#!{python}
import json, os, sys
open(os.environ["FAKE_CALLS"], "a").write(json.dumps({{"who": "signer", "argv": sys.argv[1:],
    "cwd": os.getcwd(), "has_r2_secret": "R2_SECRET_ACCESS_KEY" in os.environ, "has_gh_token": "GH_TOKEN" in os.environ}}) + "\\n")
if "--out" in sys.argv and not int(os.environ.get("FAKE_SIGNER_EXIT", "0")):
    open(sys.argv[sys.argv.index("--out") + 1], "x").write(os.environ.get("FAKE_EXTRA_BODY", "[]"))
sys.exit(int(os.environ.get("FAKE_SIGNER_EXIT", "0")))
"""
# Records its arguments and the rclone config the caller built, then copies the file into the served directory.
FAKE_RCLONE = """#!{python}
import json, os, shutil, sys
args = sys.argv[1:]
open(os.environ["FAKE_CALLS"], "a").write(json.dumps({{"who": "rclone", "argv": args,
    "config": {{k: v for k, v in os.environ.items() if k.startswith("RCLONE_CONFIG_SENTINEL_REMOTE_")}}}}) + "\\n")
if int(os.environ.get("FAKE_RCLONE_EXIT", "0")):
    sys.exit(int(os.environ["FAKE_RCLONE_EXIT"]))
src, dest = [a for a in args if not a.startswith("-")][-2:]
if "--ignore-existing" in args and os.path.exists(os.path.join(os.environ["FAKE_SERVE"], os.path.basename(dest))):
    sys.exit(0)  # the real one skips a destination that exists
if os.environ.get("FAKE_SIDECAR_BYTES"):
    open(os.path.join(os.environ["FAKE_SERVE"], os.path.basename(dest)), "w").write(os.environ["FAKE_SIDECAR_BYTES"])
else:
    shutil.copy(src, os.path.join(os.environ["FAKE_SERVE"], os.path.basename(dest)))
"""


class Quiet(http.server.SimpleHTTPRequestHandler):
    """Serves FAKE_SERVE; an HTML 200 or a redirect where the test asks for one."""

    mode = "plain"

    def log_message(self, *args) -> None:
        pass

    def do_GET(self) -> None:
        if self.mode == "html":
            body = b"<html>nothing here</html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.mode == "redirect":
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:1/elsewhere")
            self.end_headers()
        else:
            super().do_GET()


class PublishFixture(unittest.TestCase):
    """The fixture the main and the variant tests share: a built APK and sidecar, a served directory, a fake signer and a
    fake rclone, and a public host on 127.0.0.1."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = pathlib.Path(self.tmp.name)
        self.build = base / "build" / "quest-release" / TAG
        self.build.mkdir(parents=True)
        (self.build / APK_NAME).write_bytes(APK_BYTES)
        self.sum = hashlib.sha256(APK_BYTES).hexdigest()
        (self.build / f"{APK_NAME}.sha256").write_text(f"{self.sum}  {APK_NAME}\n")
        self.served = base / "served"
        (self.served / "nevr-runtime" / TAG).mkdir(parents=True)
        self.bin = base / "bin"
        self.bin.mkdir()
        self.signer_dir = base / "signer-checkout"
        (self.signer_dir / "config").mkdir(parents=True)
        (self.signer_dir / "config" / "manifest.yml").write_text("settings\n")
        (self.signer_dir / "config" / "sources.yml").write_text("sources\n")
        for name, text in (("signer", FAKE_SIGNER), ("rclone", FAKE_RCLONE)):
            path = self.bin / name
            path.write_text(text.format(python=sys.executable))
            path.chmod(0o755)
        self.calls = base / "calls.jsonl"
        self.state = base / "state"
        Quiet.mode = "plain"
        handler = lambda *a, **k: Quiet(*a, directory=str(self.served), **k)  # noqa: E731
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.env = {
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "HOME": str(base),
            "XDG_STATE_HOME": str(self.state),
            "RELEASE_PUBLIC_BASE": f"http://127.0.0.1:{self.server.server_address[1]}",
            "RELEASE_SIGNER_BIN": str(self.bin / "signer"),
            "RELEASE_SIGNER_DIR": str(self.signer_dir),
            "FAKE_CALLS": str(self.calls),
            "FAKE_SERVE": str(self.served / "nevr-runtime" / TAG),
            **SECRETS,
        }

    def run_tool(self, *args: str, drop: tuple[str, ...] = (), **extra: str) -> subprocess.CompletedProcess:
        env = {k: v for k, v in {**self.env, **extra}.items() if k not in drop}
        return subprocess.run([sys.executable, "-I", str(TOOL), "--dir", str(self.build), *args],
                              capture_output=True, text=True, env=env, cwd=self.tmp.name)

    def serve_apk(self, data: bytes = APK_BYTES) -> None:
        (self.served / "nevr-runtime" / TAG / APK_NAME).write_bytes(data)

    def recorded(self) -> list[dict]:
        if not self.calls.exists():
            return []
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def log_lines(self) -> list[dict]:
        path = self.state / "nevr-runtime" / "publish-quest-apk.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()]


class PublishQuestApk(PublishFixture):
    def test_each_missing_variable_is_a_refusal_that_names_it_and_uploads_nothing(self) -> None:
        for name in REQUIRED:
            with self.subTest(missing=name):
                if self.calls.exists():
                    self.calls.unlink()
                result = self.run_tool(TAG, drop=(name,))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"publish-quest-apk: refused: set {name}\n", result.stderr)
                for other in REQUIRED:
                    if other != name:
                        self.assertNotIn(other, result.stderr)
                self.assertEqual(self.recorded(), [])

    def test_an_empty_variable_is_a_missing_one(self) -> None:
        result = self.run_tool(TAG, R2_BUCKET="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("R2_BUCKET", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_every_missing_variable_is_named_at_once(self) -> None:
        result = self.run_tool(TAG, drop=("R2_ENDPOINT", "GH_TOKEN"))
        self.assertIn("R2_ENDPOINT", result.stderr)
        self.assertIn("GH_TOKEN", result.stderr)

    def test_the_rclone_remote_is_required_and_must_be_a_name(self) -> None:
        result = self.run_tool(TAG, drop=("NEVR_RCLONE_REMOTE",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("NEVR_RCLONE_REMOTE", result.stderr)
        self.assertEqual(self.recorded(), [])
        result = self.run_tool(TAG, NEVR_RCLONE_REMOTE="2 bad remote")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("NEVR_RCLONE_REMOTE must be letters", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_tag_that_is_not_a_release_tag_is_refused(self) -> None:
        for bad in ("v1.2", "1.2.3", "v1.2.3-dev.4", "main", "v1.2.3\n"):
            with self.subTest(tag=bad):
                result = self.run_tool(bad)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("is not a release tag", result.stderr)
                self.assertEqual(self.recorded(), [])

    def test_a_public_base_that_is_not_https_is_refused_unless_it_is_loopback(self) -> None:
        result = self.run_tool(TAG, RELEASE_PUBLIC_BASE="http://evr.example.invalid")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RELEASE_PUBLIC_BASE", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_signer_directory_without_its_config_is_refused_before_any_upload(self) -> None:
        (self.signer_dir / "config" / "sources.yml").unlink()
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RELEASE_SIGNER_DIR has no config/sources.yml", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_missing_apk_is_refused_before_any_upload(self) -> None:
        (self.build / APK_NAME).unlink()
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(APK_NAME, result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_sidecar_that_does_not_match_the_apk_is_refused_before_any_upload(self) -> None:
        (self.build / f"{APK_NAME}.sha256").write_text(f"{'0' * 64}  {APK_NAME}\n")
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sidecar", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_missing_sidecar_is_refused_before_any_upload(self) -> None:
        (self.build / f"{APK_NAME}.sha256").unlink()
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.recorded(), [])

    def test_the_apk_goes_up_through_the_signer_and_the_sidecar_through_rclone_without_overwriting(self) -> None:
        self.serve_apk()
        result = self.run_tool(TAG)
        self.assertEqual(result.returncode, 0, result.stderr)
        signer, rclone = self.recorded()
        self.assertEqual(signer["who"], "signer")
        self.assertEqual(signer["argv"][0], "put-quest-apk")
        self.assertEqual(signer["argv"][signer["argv"].index("--tag") + 1], TAG)
        self.assertEqual(signer["argv"][signer["argv"].index("--apk") + 1], str(self.build / APK_NAME))
        self.assertTrue(signer["has_r2_secret"] and signer["has_gh_token"])
        self.assertEqual(os.path.realpath(signer["cwd"]), os.path.realpath(self.signer_dir))
        self.assertEqual(rclone["who"], "rclone")
        self.assertEqual(rclone["argv"][0], "copyto")
        self.assertIn("--ignore-existing", rclone["argv"])
        self.assertEqual(rclone["argv"][-1], f"{SECRETS['NEVR_RCLONE_REMOTE']}:{SECRETS['R2_BUCKET']}/nevr-runtime/{TAG}/{APK_NAME}.sha256")
        self.assertEqual(rclone["config"]["RCLONE_CONFIG_SENTINEL_REMOTE_TYPE"], "s3")
        self.assertEqual(rclone["config"]["RCLONE_CONFIG_SENTINEL_REMOTE_ENDPOINT"], SECRETS["R2_ENDPOINT"])

    def test_it_succeeds_only_after_both_files_are_fetched_back_equal(self) -> None:
        self.serve_apk()
        result = self.run_tool(TAG)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{APK_NAME} {self.sum} {len(APK_BYTES)}", result.stdout)
        events = [line["event"] for line in self.log_lines()]
        self.assertEqual(events[0], "start")
        self.assertEqual(events[-1], "published")
        self.assertEqual(events.count("fetch"), 2)

    def test_the_public_host_serving_other_bytes_is_a_failure(self) -> None:
        self.serve_apk(b"x" * len(APK_BYTES))
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sha256", result.stderr)
        self.assertIn(APK_NAME, result.stderr)

    def test_the_public_host_serving_a_shorter_file_is_a_failure(self) -> None:
        self.serve_apk(APK_BYTES[:-1])
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"is {len(APK_BYTES) - 1} bytes, the local file is {len(APK_BYTES)}", result.stderr)

    def test_a_200_html_answer_is_a_failure(self) -> None:
        Quiet.mode = "html"
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("text/html", result.stderr)

    def test_a_redirect_is_a_failure_and_is_not_followed(self) -> None:
        Quiet.mode = "redirect"
        result = self.run_tool(TAG)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("redirect", result.stderr)

    def test_a_sidecar_on_the_host_that_differs_from_the_local_one_is_a_failure(self) -> None:
        self.serve_apk()
        result = self.run_tool(TAG, FAKE_SIDECAR_BYTES=f"{'1' * 64}  {APK_NAME}\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(".sha256", result.stderr)

    def test_a_failing_signer_stops_before_the_sidecar_and_the_fetch(self) -> None:
        self.serve_apk()
        result = self.run_tool(TAG, FAKE_SIGNER_EXIT="3")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([call["who"] for call in self.recorded()], ["signer"])
        self.assertNotIn("fetch", [line["event"] for line in self.log_lines()])

    def test_a_failing_rclone_is_a_failure(self) -> None:
        self.serve_apk()
        result = self.run_tool(TAG, FAKE_RCLONE_EXIT="4")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("fetch", [line["event"] for line in self.log_lines()])

    def test_no_secret_reaches_the_output_or_the_log(self) -> None:
        self.serve_apk()
        result = self.run_tool(TAG)
        failed = self.run_tool(TAG, FAKE_SIGNER_EXIT="3")
        log = (self.state / "nevr-runtime" / "publish-quest-apk.jsonl").read_text()
        for value in SECRETS.values():
            for text in (result.stdout, result.stderr, failed.stdout, failed.stderr, log):
                self.assertNotIn(value, text)

    def test_a_log_that_cannot_be_written_is_an_error(self) -> None:
        self.serve_apk()
        blocker = pathlib.Path(self.tmp.name) / "blocker"
        blocker.write_text("a file where the state directory should be")
        result = self.run_tool(TAG, XDG_STATE_HOME=str(blocker))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("log", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_extra_out_keeps_the_signers_manifest_claim_in_a_new_file(self) -> None:
        self.serve_apk()
        claim = json.dumps([{"name": APK_NAME, "sha256": self.sum, "size": len(APK_BYTES)}])
        target = pathlib.Path(self.tmp.name) / "claim.json"
        result = self.run_tool(TAG, "--extra-out", str(target), FAKE_EXTRA_BODY=claim)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(target.read_text(), claim)

    def test_extra_out_is_not_written_unless_asked(self) -> None:
        self.serve_apk()
        result = self.run_tool(TAG, FAKE_EXTRA_BODY="[1]")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(pathlib.Path(self.tmp.name).glob("claim*.json")), [])

    def test_an_extra_out_that_exists_is_refused_before_any_upload(self) -> None:
        target = pathlib.Path(self.tmp.name) / "claim.json"
        target.write_text("kept")
        result = self.run_tool(TAG, "--extra-out", str(target))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("claim.json", result.stderr)
        self.assertEqual(target.read_text(), "kept")
        self.assertEqual(self.recorded(), [])

    def test_a_failed_publish_writes_no_extra_out(self) -> None:
        target = pathlib.Path(self.tmp.name) / "claim.json"
        result = self.run_tool(TAG, "--extra-out", str(target), FAKE_SIGNER_EXIT="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(target.exists())


class PublishVariantApk(PublishFixture):
    """`--variant NAME`: an unlisted APK of the release goes up under nevr-runtime/<tag>/ by `rclone copyto
    --ignore-existing` and is fetched back; the signer is never run, so there is no manifest claim."""

    def setUp(self) -> None:
        super().setUp()
        self.vbuild = self.build / "variant"
        self.vbuild.mkdir()
        (self.vbuild / VARIANT_NAME).write_bytes(VARIANT_BYTES)
        self.vsum = hashlib.sha256(VARIANT_BYTES).hexdigest()
        (self.vbuild / f"{VARIANT_NAME}.sha256").write_text(f"{self.vsum}  {VARIANT_NAME}\n")
        # A signer that must not be run: it fails when it is.
        self.env["FAKE_SIGNER_EXIT"] = "9"

    def run_variant(self, *args: str, drop: tuple[str, ...] = (), **extra: str) -> subprocess.CompletedProcess:
        env = {k: v for k, v in {**self.env, **extra}.items() if k not in drop}
        return subprocess.run([sys.executable, "-I", str(TOOL), TAG, "--variant", VARIANT, "--dir", str(self.vbuild), *args],
                              capture_output=True, text=True, env=env, cwd=self.tmp.name)

    def serve_variant(self, data: bytes = VARIANT_BYTES) -> None:
        (self.served / "nevr-runtime" / TAG / VARIANT_NAME).write_bytes(data)

    def test_the_variant_goes_up_by_rclone_without_overwriting_and_the_signer_is_never_run(self) -> None:
        result = self.run_variant()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.recorded()
        self.assertEqual([call["who"] for call in calls], ["rclone", "rclone"], "no signer call, so no manifest claim")
        apk_up, sidecar_up = calls
        for call in calls:
            self.assertEqual(call["argv"][0], "copyto")
            self.assertIn("--ignore-existing", call["argv"])
            self.assertEqual(call["config"]["RCLONE_CONFIG_SENTINEL_REMOTE_TYPE"], "s3")
        self.assertEqual(apk_up["argv"][-2], str(self.vbuild / VARIANT_NAME))
        self.assertEqual(apk_up["argv"][-1], f"{SECRETS['NEVR_RCLONE_REMOTE']}:{SECRETS['R2_BUCKET']}/nevr-runtime/{TAG}/{VARIANT_NAME}")
        self.assertEqual(sidecar_up["argv"][-1], f"{SECRETS['NEVR_RCLONE_REMOTE']}:{SECRETS['R2_BUCKET']}/nevr-runtime/{TAG}/{VARIANT_NAME}.sha256")
        self.assertEqual((self.served / "nevr-runtime" / TAG / VARIANT_NAME).read_bytes(), VARIANT_BYTES)
        self.assertIn(f"{VARIANT_NAME} {self.vsum} {len(VARIANT_BYTES)}", result.stdout)

    def test_the_log_has_one_record_per_step_and_each_names_the_variant(self) -> None:
        self.assertEqual(self.run_variant().returncode, 0)
        lines = self.log_lines()
        self.assertEqual([line["event"] for line in lines],
                         ["start", "local_files", "put_variant_apk", "put_sidecar", "fetch", "fetch", "published"])
        for line in lines:
            self.assertEqual(line["variant"], VARIANT, line)
        self.assertEqual(lines[2]["object"], f"nevr-runtime/{TAG}/{VARIANT_NAME}")
        self.assertEqual(lines[3]["object"], f"nevr-runtime/{TAG}/{VARIANT_NAME}.sha256")
        self.assertEqual([line["ok"] for line in lines if line["event"] == "fetch"], [True, True])
        self.assertEqual((lines[-1]["sha256"], lines[-1]["size"]), (self.vsum, len(VARIANT_BYTES)))
        self.assertNotIn("put_apk", [line["event"] for line in lines])

    def test_a_main_publish_records_no_variant(self) -> None:
        self.serve_apk()
        self.assertEqual(self.run_tool(TAG, FAKE_SIGNER_EXIT="0").returncode, 0)
        self.assertTrue(all("variant" not in line for line in self.log_lines()))

    def test_the_variant_needs_no_signer_and_no_github_token(self) -> None:
        result = self.run_variant(drop=("RELEASE_SIGNER_BIN", "RELEASE_SIGNER_DIR", "GH_TOKEN"))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_missing_r2_or_host_variable_is_a_refusal_that_names_it(self) -> None:
        for name in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "RELEASE_PUBLIC_BASE", "NEVR_RCLONE_REMOTE"):
            with self.subTest(missing=name):
                if self.calls.exists():
                    self.calls.unlink()
                result = self.run_variant(drop=(name,))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"publish-quest-apk: refused: set {name}\n", result.stderr)
                self.assertEqual(self.recorded(), [])

    def test_the_variant_default_directory_is_the_variant_subdirectory_of_the_release_build(self) -> None:
        result = subprocess.run([sys.executable, "-I", str(TOOL), TAG, "--variant", VARIANT],
                                capture_output=True, text=True, env=self.env, cwd=self.tmp.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(VARIANT_NAME, result.stdout)

    def test_extra_out_with_a_variant_is_refused_before_anything_is_done(self) -> None:
        target = pathlib.Path(self.tmp.name) / "claim.json"
        result = self.run_variant("--extra-out", str(target))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--extra-out", result.stderr)
        self.assertIn("--variant", result.stderr)
        self.assertFalse(target.exists())
        self.assertEqual(self.recorded(), [])
        self.assertNotIn("local_files", [line["event"] for line in self.log_lines()])

    def test_an_empty_extra_out_with_a_variant_is_refused_too(self) -> None:
        result = self.run_variant("--extra-out", "")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--extra-out", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_variant_name_outside_lowercase_letters_and_digits_is_refused(self) -> None:
        for bad in ("Q15T", "q-15", "q 15", "", "q15t/../x", "q15t\n", "q_15"):
            with self.subTest(name=bad):
                result = subprocess.run([sys.executable, "-I", str(TOOL), TAG, "--variant", bad, "--dir", str(self.vbuild)],
                                        capture_output=True, text=True, env=self.env, cwd=self.tmp.name)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("--variant", result.stderr)
                self.assertEqual(self.recorded(), [])

    def test_a_missing_local_apk_is_a_failure_that_names_it_and_uploads_nothing(self) -> None:
        (self.vbuild / VARIANT_NAME).unlink()
        result = self.run_variant()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(VARIANT_NAME, result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_missing_sidecar_is_a_failure_and_uploads_nothing(self) -> None:
        (self.vbuild / f"{VARIANT_NAME}.sha256").unlink()
        result = self.run_variant()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{VARIANT_NAME}.sha256", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_sidecar_that_does_not_match_the_apk_is_refused_before_any_upload(self) -> None:
        (self.vbuild / f"{VARIANT_NAME}.sha256").write_text(f"{'0' * 64}  {VARIANT_NAME}\n")
        result = self.run_variant()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sidecar", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_a_sidecar_that_names_another_file_is_refused_even_with_the_right_digest(self) -> None:
        (self.vbuild / f"{VARIANT_NAME}.sha256").write_text(f"{self.vsum}  {APK_NAME}\n")
        result = self.run_variant()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sidecar", result.stderr)
        self.assertEqual(self.recorded(), [])

    def test_an_object_already_on_the_host_with_other_bytes_is_not_replaced_and_fails_the_fetch_back(self) -> None:
        self.serve_variant(b"x" * len(VARIANT_BYTES))
        result = self.run_variant()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sha256", result.stderr)
        self.assertIn(VARIANT_NAME, result.stderr)
        self.assertEqual((self.served / "nevr-runtime" / TAG / VARIANT_NAME).read_bytes(), b"x" * len(VARIANT_BYTES))
        self.assertEqual([line["ok"] for line in self.log_lines() if line["event"] == "fetch"], [False])
        self.assertNotIn("published", [line["event"] for line in self.log_lines()])

    def test_an_object_already_on_the_host_with_the_same_bytes_is_a_clean_rerun(self) -> None:
        self.serve_variant()
        result = self.run_variant()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.log_lines()[-1]["event"], "published")

    def test_the_host_serving_a_shorter_file_is_a_failure(self) -> None:
        self.serve_variant(VARIANT_BYTES[:-1])
        result = self.run_variant()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"is {len(VARIANT_BYTES) - 1} bytes, the local file is {len(VARIANT_BYTES)}", result.stderr)

    def test_a_200_html_answer_and_a_redirect_are_failures(self) -> None:
        for mode, word in (("html", "text/html"), ("redirect", "redirect")):
            with self.subTest(mode=mode):
                Quiet.mode = mode
                result = self.run_variant()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(word, result.stderr)

    def test_a_sidecar_on_the_host_that_differs_from_the_local_one_is_a_failure(self) -> None:
        self.serve_variant()
        result = self.run_variant(FAKE_SIDECAR_BYTES=f"{'1' * 64}  {VARIANT_NAME}\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(".sha256", result.stderr)
        self.assertEqual([line["ok"] for line in self.log_lines() if line["event"] == "fetch"], [True, False])

    def test_a_failing_rclone_is_a_failure_and_nothing_is_fetched(self) -> None:
        result = self.run_variant(FAKE_RCLONE_EXIT="4")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("fetch", [line["event"] for line in self.log_lines()])
        self.assertEqual(self.log_lines()[-1]["event"], "failed")
        self.assertEqual(self.log_lines()[-1]["variant"], VARIANT)

    def test_no_secret_reaches_the_output_or_the_log(self) -> None:
        ok = self.run_variant()
        failed = self.run_variant(FAKE_RCLONE_EXIT="4")
        log = (self.state / "nevr-runtime" / "publish-quest-apk.jsonl").read_text()
        for value in SECRETS.values():
            for text in (ok.stdout, ok.stderr, failed.stdout, failed.stderr, log):
                self.assertNotIn(value, text)


class Wiring(unittest.TestCase):
    def test_the_recipe_runs_the_tool_with_the_tag(self) -> None:
        text = (ROOT / "justfile").read_text(encoding="utf-8")
        self.assertRegex(text, r'(?m)^publish-quest-apk tag:\n    python3 tools/publish_quest_apk\.py "\{\{ tag \}\}"$')

    def test_releasing_md_points_at_the_recipe_and_the_page_that_names_the_host(self) -> None:
        text = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
        section = text.split("## The Quest APK", 1)[1].split("## Immutable releases", 1)[0]
        self.assertIn("just publish-quest-apk vX.Y.Z", section)
        self.assertIn("docs/reference/verifying-a-release.md", section)
        self.assertNotIn("nevr-runtime/quest/", text)
        self.assertNotIn("echo.taxi", text)

    def test_the_verification_page_names_the_host_the_versioned_path_and_the_bare_host_warning(self) -> None:
        text = (ROOT / "docs" / "reference" / "verifying-a-release.md").read_text(encoding="utf-8")
        section = text.split("## Quest APKs", 1)[1].split("\n## ", 1)[0]
        self.assertIn("evr.echo.taxi/nevr-runtime/v<version>/nevr-runtime-v<version>-quest.apk", section)
        self.assertIn("bare `echo.taxi`", section)
        self.assertIn("200", section)


if __name__ == "__main__":
    unittest.main()
