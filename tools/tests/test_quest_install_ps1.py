"""tools/quest-install.ps1 against a fake adb: it never touches a headset or the network."""
from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/quest-install.ps1"
DATA = b"test data zip"
DATA_SHA = hashlib.sha256(DATA).hexdigest()
APK = b"test apk"
APK_SHA = hashlib.sha256(APK).hexdigest()

# Behaviour comes from FAKE_* env; every call after `devices` is appended to FAKE_ADB_LOG with the
# leading `-s <serial>` removed.
FAKE_ADB = r"""#!/usr/bin/env bash
if [[ "$1" == devices ]]; then printf 'List of devices attached\n%b' "${FAKE_DEVICES-ABC123\tdevice\n}"; exit 0; fi
shift 2
printf '%s\n' "$*" >> "$FAKE_ADB_LOG"
case "$1" in
  wait-for-device) exit 0 ;;
  install)
    if [[ "$2" == -r && -n "${FAKE_INCOMPATIBLE:-}" ]]; then echo 'Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]'; exit 1; fi
    echo Success; exit 0 ;;
  uninstall) echo Success; exit 0 ;;
  push) exit "${FAKE_PUSH_RC:-0}" ;;
  shell)
    case "$2" in
      pm) printf '%s' "${FAKE_PM_OUT-}"; exit "${FAKE_PM_RC:-1}" ;;
      df*) printf 'Filesystem 1024-blocks Used Available Capacity Mounted\n/dev/x 9000000 1000 8000000 1%% /sdcard\n' ;;
      *) exit 0 ;;
    esac ;;
esac
"""


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class QuestInstallPs1Test(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="quest-install-ps1-test-", dir="/var/tmp"))
        self.addCleanup(self.cleanup)
        self.local = self.tmp / "LocalAppData"
        cache = self.local / "nevr" / "quest-data"
        cache.mkdir(parents=True)
        (cache / f"_data-{DATA_SHA}.zip").write_bytes(DATA)
        self.apk = self.tmp / "sample quest.apk"
        self.apk.write_bytes(APK)
        self.adb = self.tmp / "fake-adb"
        self.adb.write_text(FAKE_ADB)
        self.adb.chmod(0o755)
        self.log = self.tmp / "adb.log"

    def cleanup(self):
        for path in self.tmp.rglob("*"):
            if path.is_dir():
                path.chmod(0o755)
        shutil.rmtree(self.tmp)

    def run_script(self, *args, adb=True, apk_sha=APK_SHA, env=None, stdin="\n", pause=False):
        cmd = ["pwsh", "-NoProfile", "-File", str(SCRIPT), "-Apk", str(self.apk), "-DataSha256", DATA_SHA]
        if apk_sha:
            cmd += ["-ApkSha256", apk_sha]
        if adb:
            cmd += ["-Adb", str(self.adb)]
        if not pause:
            cmd.append("-NoPause")
        cmd += list(args)
        full_env = dict(os.environ, LOCALAPPDATA=str(self.local), APPDATA=str(self.tmp / "Roaming"),
                        FAKE_ADB_LOG=str(self.log))
        full_env.update(env or {})
        return subprocess.run(cmd, env=full_env, input=stdin, capture_output=True, text=True, timeout=60)

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_a_full_install_runs_the_same_sequence_as_the_shell_installer(self):
        result = self.run_script(env={"FAKE_PM_RC": "0", "FAKE_PM_OUT": "package:/x"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Done.", result.stdout)
        calls = self.calls()
        wanted = ["wait-for-device", "shell pm path com.readyatdawn.r15", "shell df -Pk /sdcard",
                  "shell df -Pk /data/local/tmp", f"install -r -g {self.apk}", "push ",
                  "unzip -o /data/local/tmp/_data.zip", "rm -f /data/local/tmp/_data.zip"]
        position = -1
        for needle in wanted:
            found = [i for i, c in enumerate(calls) if needle in c and i > position]
            self.assertTrue(found, f"{needle!r} missing or out of order in {calls}")
            position = found[0]

    def test_a_headset_without_the_game_installs_instead_of_aborting(self):
        # `pm path` exits 1 with no output when the package is absent (quest-install.sh treats that as normal).
        result = self.run_script(env={"FAKE_PM_RC": "1", "FAKE_PM_OUT": ""})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("is not installed on ABC123", result.stdout)
        self.assertTrue(any(c.startswith("install -r -g") for c in self.calls()), self.calls())

    def test_a_failing_package_check_is_an_error(self):
        result = self.run_script(env={"FAKE_PM_RC": "7", "FAKE_PM_OUT": "Error: boom"})
        self.assertEqual(result.returncode, 1)
        self.assertIn("package check failed (exit 7): Error: boom", result.stderr)
        self.assertFalse(any(c.startswith("install") for c in self.calls()))

    def test_no_device_one_device_and_authorization_are_named(self):
        none = self.run_script(env={"FAKE_DEVICES": ""})
        self.assertEqual(none.returncode, 1)
        self.assertIn("No adb device attached", none.stderr)
        two = self.run_script(env={"FAKE_DEVICES": "ABC123\\tdevice\\nXYZ987\\tdevice\\n"})
        self.assertEqual(two.returncode, 1)
        self.assertIn("2 adb devices attached", two.stderr)
        unauthorized = self.run_script(env={"FAKE_DEVICES": "ABC123\\tunauthorized\\n"})
        self.assertEqual(unauthorized.returncode, 1)
        self.assertIn("is unauthorized", unauthorized.stderr)
        self.assertEqual(self.calls(), [])

    def test_a_device_without_permissions_is_named_not_reported_as_state_no(self):
        result = self.run_script(env={"FAKE_DEVICES": "ABC123\\tno permissions (missing udev rules?); see [x]\\n"})
        self.assertEqual(result.returncode, 1)
        self.assertIn("no permissions", result.stderr)
        self.assertIn("usually missing udev rules", result.stderr)
        self.assertNotIn("state 'no'", result.stderr)

    def test_a_wrong_apk_hash_stops_before_adb_runs(self):
        result = self.run_script(apk_sha="0" * 64)
        self.assertEqual(result.returncode, 1)
        self.assertIn("SHA-256 mismatch", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_sha256sums_next_to_the_apk_is_the_expected_hash(self):
        (self.tmp / "SHA256SUMS").write_text(f"{APK_SHA}  sample quest.apk\n")
        result = self.run_script(apk_sha="", env={"FAKE_PM_RC": "0", "FAKE_PM_OUT": "package:/x"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_an_incompatible_signature_needs_yes_before_anything_is_uninstalled(self):
        refused = self.run_script(env={"FAKE_INCOMPATIBLE": "1"})
        self.assertEqual(refused.returncode, 1, refused.stdout + refused.stderr)
        self.assertIn("Re-run with -Yes", refused.stderr)
        self.assertFalse(any(c.startswith("uninstall") for c in self.calls()))
        self.log.unlink()
        allowed = self.run_script("-Yes", env={"FAKE_INCOMPATIBLE": "1"})
        self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)
        calls = self.calls()
        self.assertTrue(any(c == "uninstall com.readyatdawn.r15" for c in calls), calls)
        self.assertTrue(any(c == f"install -g {self.apk}" for c in calls), calls)

    def test_the_temp_zip_is_removed_even_when_the_push_fails(self):
        result = self.run_script(env={"FAKE_PUSH_RC": "1"})
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any("rm -f /data/local/tmp/_data.zip" in c for c in self.calls()), self.calls())

    def test_a_corrupt_cached_zip_is_deleted_and_not_pushed(self):
        cached = self.local / "nevr" / "quest-data" / f"_data-{DATA_SHA}.zip"
        cached.write_bytes(b"corrupt")
        result = self.run_script(env={"FAKE_PM_RC": "0", "FAKE_PM_OUT": "package:/x"})
        self.assertEqual(result.returncode, 1)
        self.assertIn("has the wrong SHA-256; deleting it", result.stdout)
        self.assertFalse(cached.exists())
        self.assertFalse(any(c.startswith("push") for c in self.calls()))

    @unittest.skipIf(os.geteuid() == 0, "root reads every folder")
    def test_discovery_survives_a_folder_the_user_cannot_read(self):
        blocked = self.local / "Blocked"
        (blocked / "inner").mkdir(parents=True)
        blocked.chmod(0)
        platform_tools = self.local / "SideQuest" / "platform-tools"
        platform_tools.mkdir(parents=True)
        shutil.copy(self.adb, platform_tools / "adb.exe")
        result = self.run_script(adb=False, env={"FAKE_PM_RC": "0", "FAKE_PM_OUT": "package:/x"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(str(platform_tools / "adb.exe"), result.stdout)

    def test_the_window_waits_for_enter_after_success_and_after_an_error(self):
        ok = self.run_script(pause=True, env={"FAKE_PM_RC": "0", "FAKE_PM_OUT": "package:/x"})
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
        self.assertIn("Press Enter to close", ok.stdout)
        self.assertTrue(ok.stdout.rstrip().endswith("Press Enter to close") or "Done." in ok.stdout)
        failed = self.run_script(pause=True, env={"FAKE_DEVICES": ""})
        self.assertEqual(failed.returncode, 1)
        self.assertIn("No adb device attached", failed.stderr)
        self.assertIn("Press Enter to close", failed.stdout)
        quiet = self.run_script(env={"FAKE_PM_RC": "0", "FAKE_PM_OUT": "package:/x"})
        self.assertNotIn("Press Enter", quiet.stdout)


if __name__ == "__main__":
    unittest.main()
