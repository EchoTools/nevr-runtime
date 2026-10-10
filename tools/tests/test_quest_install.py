"""tools/quest-install.sh against a fake adb and curl: it never touches a headset or the network."""
from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/quest-install.sh"
DEFAULT_SHA = "fc2eedeacc50d9ddf751e21914bb4188660cf5e79ce48aab24b84e757f4b543c"
BODY = b"game-data-zip"
BODY_SHA = hashlib.sha256(BODY).hexdigest()
ONE_DEVICE = "List of devices attached\nQUEST123\tdevice\n"


def make_fakes(directory: pathlib.Path) -> None:
    directory.mkdir()
    # adb: behaviour from FAKE_* env; every call (minus the leading -s serial) is appended to FAKE_ADB_LOG.
    (directory / "adb").write_text(
        '#!/bin/bash\n'
        'if [[ "$1" == "devices" ]]; then printf "%s" "${FAKE_DEVICES:-}"; exit 0; fi\n'
        '[[ "$1" == "-s" ]] && shift 2\n'
        'echo "$*" >> "$FAKE_ADB_LOG"\n'
        'case "$1" in\n'
        '  wait-for-device) exit 0 ;;\n'
        '  install) printf "%s\\n" "${FAKE_INSTALL_OUT:-Success}"; exit "${FAKE_INSTALL_RC:-0}" ;;\n'
        '  uninstall) exit "${FAKE_UNINSTALL_RC:-0}" ;;\n'
        '  push) exit "${FAKE_PUSH_RC:-0}" ;;\n'
        '  shell)\n'
        '    case "$2" in\n'
        '      pm) printf "%s" "${FAKE_PM_OUT-package:/x}"; exit "${FAKE_PM_RC:-0}" ;;\n'
        '      df*) printf "Filesystem 1K-blocks Used Available Use%% Mounted\\nx 1 1 %s 1%% /\\n" "${FAKE_AVAIL_KB:-4000000}" ;;\n'
        '      *unzip*) exit "${FAKE_UNZIP_RC:-0}" ;;\n'
        '      *) exit 0 ;;\n'
        '    esac ;;\n'
        'esac\n')
    # timeout: log "<seconds> <command...>" and then really run it, unless a hung pm path is being faked.
    real_timeout = shutil.which("timeout")
    (directory / "timeout").write_text(
        '#!/bin/bash\n'
        'echo "$*" >> "$FAKE_TIMEOUT_LOG"\n'
        '[[ -n "${FAKE_PM_TIMEOUT:-}" && "$*" == *"pm path"* ]] && exit 124\n'
        f'exec {real_timeout} "$@"\n')
    (directory / "curl").write_text(
        '#!/bin/bash\n'
        'echo "$*" >> "$FAKE_CURL_LOG"\n'
        '[[ -n "${FAKE_CURL_FAIL:-}" ]] && exit 22\n'
        'out=""; while [[ $# -gt 0 ]]; do [[ "$1" == "-o" ]] && out="$2"; shift; done\n'
        'printf "%s" "$FAKE_CURL_BODY" > "$out"\n')
    for f in directory.iterdir():
        f.chmod(0o755)


class QuestInstallTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="quest-install-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        make_fakes(self.tmp / "bin")
        self.work = self.tmp / "work"
        self.work.mkdir()
        self.apk = self.work / "r15.apk"
        self.apk.write_bytes(b"apk")
        self.adb_log = self.tmp / "adb.log"
        self.timeout_log = self.tmp / "timeout.log"
        self.curl_log = self.tmp / "curl.log"
        for log in (self.adb_log, self.timeout_log, self.curl_log):
            log.write_text("")
        self.env = dict(
            os.environ,
            PATH=f"{self.tmp / 'bin'}:{os.environ['PATH']}",
            FAKE_DEVICES=ONE_DEVICE,
            FAKE_ADB_LOG=str(self.adb_log),
            FAKE_TIMEOUT_LOG=str(self.timeout_log),
            FAKE_CURL_LOG=str(self.curl_log),
            FAKE_CURL_BODY=BODY.decode(),
        )

    def run_script(self, yes="", sha=BODY_SHA, url="https://example.invalid/_data.zip", env=None):
        return subprocess.run([str(SCRIPT), yes, str(self.apk), url, sha], env=env or self.env, cwd=self.work,
                              capture_output=True, text=True, timeout=60)

    def adb_calls(self):
        return self.adb_log.read_text().splitlines()

    def test_a_full_install_downloads_pushes_and_extracts(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Done.", result.stdout)
        calls = self.adb_calls()
        self.assertTrue(any(c.startswith("install -r -g") for c in calls), calls)
        self.assertTrue(any(c.startswith("push") for c in calls), calls)
        self.assertTrue(any("unzip -o" in c for c in calls), calls)
        self.assertTrue(any("rm -f /data/local/tmp/_data.zip" in c for c in calls), "temp zip not cleaned up")

    def test_the_download_has_a_stall_and_total_time_limit(self):
        self.run_script()
        curl = self.curl_log.read_text()
        for flag in ("--speed-limit 10000", "--speed-time 60", "--max-time 7200", "--retry-max-time 7200",
                     "--connect-timeout 20", "--max-filesize"):
            self.assertIn(flag, curl)

    def test_the_free_space_threshold_is_exactly_2_2_gib(self):
        self.assertEqual(self.run_script(env=dict(self.env, FAKE_AVAIL_KB="2306867")).returncode, 0)
        refused = self.run_script(env=dict(self.env, FAKE_AVAIL_KB="2306866"))
        self.assertEqual(refused.returncode, 1)
        self.assertIn("free, need 2252 MiB", refused.stderr)

    def test_adb_never_reads_the_terminal_under_timeout(self):
        # adb runs in timeout's own process group; every adb call must have stdin from /dev/null.
        text = SCRIPT.read_text()
        import re
        calls = [l for l in text.splitlines() if re.search(r"timeout (30|\"\$secs\") adb", l)]
        self.assertGreaterEqual(len(calls), 5)
        for line in calls:
            self.assertIn("</dev/null", line, line)

    def test_package_state_branches(self):
        installed = self.run_script(env=dict(self.env, FAKE_PM_RC="0", FAKE_PM_OUT="package:/x"))
        self.assertIn("is installed on", installed.stdout)
        absent = self.run_script(env=dict(self.env, FAKE_PM_RC="1", FAKE_PM_OUT=""))
        self.assertEqual(absent.returncode, 0, absent.stdout + absent.stderr)
        self.assertIn("is not installed on", absent.stdout)
        broken = self.run_script(env=dict(self.env, FAKE_PM_RC="7", FAKE_PM_OUT="Error: boom"))
        self.assertEqual(broken.returncode, 1)
        self.assertIn("package check failed (exit 7): Error: boom", broken.stderr)

    def test_a_device_without_permissions_is_named_not_hidden(self):
        denied = "List of devices attached\nQUEST123\tno permissions (missing udev rules? user is in the plugdev group); see [x]\n"
        result = self.run_script(env=dict(self.env, FAKE_DEVICES=denied))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no permissions", result.stderr)
        self.assertIn("usually missing udev rules", result.stderr)

    def test_a_malformed_hash_is_rejected_before_anything_is_touched(self):
        for bad in ("abc", "x/../_data", "F" * 64, ""):
            result = self.run_script(sha=bad)
            self.assertNotEqual(result.returncode, 0, bad)
        self.assertEqual(self.curl_log.read_text(), "")
        self.assertEqual(self.adb_calls(), [])

    def test_the_pinned_hash_uses_the_pinned_cache_name(self):
        cache = self.work / "build/android-arm64/quest-data"
        cache.mkdir(parents=True)
        (cache / "_data.zip").write_bytes(b"not the pinned content")
        result = self.run_script(sha=DEFAULT_SHA)
        self.assertIn("Cached build/android-arm64/quest-data/_data.zip does not match the pinned SHA-256", result.stdout)

    def test_the_temp_zip_is_removed_even_when_the_push_fails(self):
        result = self.run_script(env=dict(self.env, FAKE_PUSH_RC="1"))
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any("rm -f /data/local/tmp/_data.zip" in c for c in self.adb_calls()), self.adb_calls())

    def test_every_long_adb_call_runs_under_timeout(self):
        self.run_script()
        timeouts = self.timeout_log.read_text()
        for secs, needle in (("600", "install -r -g"), ("1800", " push "), ("900", "unzip -o"), ("60", "rm -f")):
            self.assertRegex(timeouts, rf"(?m)^{secs} adb .*{needle}", f"{needle!r} is not bounded by {secs}s")

    def test_a_timed_out_package_check_is_an_error_not_not_installed(self):
        result = self.run_script(env=dict(self.env, FAKE_PM_TIMEOUT="1"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("package check timed out", result.stderr)
        self.assertNotIn("is not installed", result.stdout)

    def test_adb_server_warnings_are_not_counted_as_devices(self):
        noisy = ("* daemon not running; starting now at tcp:5037\n* daemon started successfully\n"
                 "adb server version (41) doesn't match this client (39); killing...\n" + ONE_DEVICE)
        result = self.run_script(env=dict(self.env, FAKE_DEVICES=noisy))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_two_real_devices_are_still_refused(self):
        two = ONE_DEVICE + "OTHER456\tdevice\n"
        result = self.run_script(env=dict(self.env, FAKE_DEVICES=two))
        self.assertEqual(result.returncode, 1)
        self.assertIn("2 adb devices attached", result.stderr)

    def test_not_enough_free_space_stops_before_any_download(self):
        result = self.run_script(env=dict(self.env, FAKE_AVAIL_KB="2000000"))  # ~1.9 GiB: below the 2.2 GiB need
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("free, need", result.stderr)
        self.assertEqual(self.curl_log.read_text(), "")

    def test_a_different_hash_never_deletes_the_pinned_cached_zip(self):
        cache = self.work / "build/android-arm64/quest-data"
        cache.mkdir(parents=True)
        pinned = cache / "_data.zip"
        pinned.write_bytes(b"a good cached zip for the pinned hash")
        result = self.run_script(sha=BODY_SHA)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(pinned.read_bytes(), b"a good cached zip for the pinned hash")
        self.assertTrue((cache / f"_data-{BODY_SHA}.zip").exists())

    def test_a_mismatching_cache_for_the_requested_hash_is_replaced(self):
        cache = self.work / "build/android-arm64/quest-data"
        cache.mkdir(parents=True)
        named = cache / f"_data-{BODY_SHA}.zip"
        named.write_bytes(b"corrupt")
        result = self.run_script(sha=BODY_SHA)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(named.read_bytes(), BODY)

    def test_a_download_with_the_wrong_hash_is_rejected_and_removed(self):
        result = self.run_script(sha="0" * 64)
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not match pinned SHA-256", result.stderr)
        self.assertEqual(list((self.work / "build/android-arm64/quest-data").glob("*.part")), [])

    def test_a_failed_download_is_an_error(self):
        result = self.run_script(env=dict(self.env, FAKE_CURL_FAIL="1"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("download failed", result.stderr)

    def test_an_incompatible_signature_needs_yes_before_anything_is_uninstalled(self):
        env = dict(self.env, FAKE_INSTALL_OUT="Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]", FAKE_INSTALL_RC="1")
        refused = self.run_script(env=env)
        self.assertEqual(refused.returncode, 1, refused.stdout + refused.stderr)
        self.assertIn("re-run with the 'yes' argument", refused.stderr)
        self.assertFalse(any(c.startswith("uninstall") for c in self.adb_calls()))

    def test_yes_allows_the_uninstall_then_installs_again(self):
        # First install fails as incompatible, the retry after the uninstall succeeds.
        counter = self.tmp / "installs"
        counter.write_text("0")
        wrapper = self.tmp / "bin" / "adb"
        original = wrapper.read_text()
        wrapper.write_text(original.replace(
            '  install) printf "%s\\n" "${FAKE_INSTALL_OUT:-Success}"; exit "${FAKE_INSTALL_RC:-0}" ;;',
            f'  install) n=$(cat {counter}); echo $((n+1)) > {counter};'
            ' if [[ $n == 0 ]]; then echo "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]"; exit 1; fi; exit 0 ;;'))
        result = self.run_script(yes="yes")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.adb_calls()
        self.assertTrue(any(c.startswith("uninstall com.readyatdawn.r15") for c in calls), calls)
        self.assertTrue(any(c == f"install -g {self.apk}" for c in calls), calls)
        timeouts = self.timeout_log.read_text()
        self.assertRegex(timeouts, r"(?m)^120 adb .*uninstall")
        self.assertRegex(timeouts, rf"(?m)^600 adb .*install -g {self.apk}")

    def test_an_unrecognized_first_argument_is_rejected(self):
        result = self.run_script(yes="maybe")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unrecognized argument", result.stderr)


if __name__ == "__main__":
    unittest.main()
