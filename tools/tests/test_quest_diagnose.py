"""quest-diagnose.ps1 and quest-diagnose.sh against a fake adb: no headset is touched (#750).

Both scripts run for real (pwsh, bash) with a fake adb answering the commands they issue. The fake logs every call
after `devices` with the leading `-s <serial>` removed. What a real headset prints for these commands is not
verified here: that is the headset check owed on the pull request.
"""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
PS1 = REPO / "tools/package-release/quest-diagnose.ps1"
SH = REPO / "tools/quest-diagnose.sh"
SERIAL = "1WMHH8XQ4R0123"
EMAIL = "player.one@example.com"
MEDIA = "/sdcard/Android/media/com.readyatdawn.r15"
LEGACY = "/sdcard/readyatdawn"
FILES = "/sdcard/Android/data/com.readyatdawn.r15/files"
GOOD_DIRS = f"{MEDIA}:{MEDIA}/files:{MEDIA}/files/_data:{MEDIA}/asset_patches:{FILES}"

HWSERIAL = "HWSER0987654"
WIFI_SERIAL = "192.168.1.5:5555"
PRIVACY = "never in a public channel"
CONTACT = "send it to a maintainer by direct message on the EchoVRCE Discord"
ZIP_FILES = {
    "verdict.txt", "device.txt", "package.txt", "installer_version.txt", "launch.txt", "logcat.txt",
    "logcat_before_launch.txt", "listings/readyatdawn.txt", "listings/readyatdawn_data.txt", "listings/media.txt",
    "listings/media_files.txt", "listings/media_files_data.txt", "listings/media_asset_patches.txt",
    "listings/android_data_files.txt", "sentinel/nevr-sentinel.log", "sentinel/nevr-sentinel.session-1.log", "sentinel/pull_results.txt",
    "sentinel/nevr-crashes/crash-1.txt", "sentinel/nevr-crashes/crash-1.dmp",
}

LOGCAT = f"""10-21 22:00:01.000  100  100 I NEVR-Sentinel: {{"event":"sentinel_ctor","account":"{EMAIL}"}}
10-21 22:00:02.000  100  100 I NEVR-Sentinel: serial {SERIAL} attached
10-21 22:00:02.500  100  100 I Settings: ro.serialno={HWSERIAL} ssid="HomeNet" bssid=aa:bb:cc:dd:ee:ff ip=192.168.1.50
10-21 22:00:03.000  100  100 I EchoVR : Loading OBB from '/sdcard/obb/main.obb'
10-21 22:00:03.100  100  100 I EchoVR : OBB mounted at '{MEDIA}/files'
10-21 22:00:03.200  100  100 I NEVR-Sentinel: {{"event":"data_root","result":"found","source":"android_media","root":"{MEDIA}/files"}}
"""

FAKE_ADB = r"""#!/usr/bin/env bash
if [[ "$1" == devices ]]; then printf 'List of devices attached\n%b' "${FAKE_DEVICES-""" + SERIAL + r"""\tdevice\n}"; exit 0; fi
[[ "$1" == -s ]] && shift 2
printf '%s\n' "$*" >> "$FAKE_ADB_LOG"
has_dir() { [[ ":$FAKE_DIRS:" == *":$1:"* ]]; }
case "$*" in
  "pull "*)
    if [[ -n "${FAKE_NO_MEDIA_LOGS:-}" && "$2" == */Android/media/* ]]; then echo "adb: error: failed to stat remote object '$2': No such file or directory"; exit 1; fi
    case "$2" in
      */nevr-sentinel.session-*.log)
        mkdir -p "$(dirname "$3")"
        printf '{"event":"sentinel_ctor","account":"%s","session":"previous"}\n' "$FAKE_EMAIL" > "$3"
        echo "1 file pulled"; exit 0 ;;
      */nevr-sentinel.log)
        if [[ -n "${FAKE_NO_SENTINEL_LOG:-}" ]]; then echo "adb: error: failed to stat remote object '$2': No such file or directory"; exit 1; fi
        mkdir -p "$(dirname "$3")"
        printf '{"event":"sentinel_ctor","account":"%s","serial":"%s"}\n' "$FAKE_EMAIL" "$FAKE_SERIAL" > "$3"
        echo "1 file pulled"; exit 0 ;;
      */nevr-crashes)
        mkdir -p "$3/nevr-crashes"
        printf 'MDMP-binary' > "$3/nevr-crashes/crash-1.dmp"
        printf 'crash for %s and %s\n' "$FAKE_SERIAL" "$FAKE_HWSERIAL" > "$3/nevr-crashes/crash-1.txt"
        echo "2 files pulled"; exit 0 ;;
      *) echo "unexpected pull $2" >&2; exit 1 ;;
    esac ;;
  "shell ls "[!-]*) printf 'nevr-sentinel.log\nnevr-sentinel.session-1.log\nnevr-crashes\nconfig.yaml\n' ;;
  "shell getprop ro.serialno") echo "$FAKE_HWSERIAL" ;;
  "shell getprop ro.product.model") echo "Quest 2" ;;
  "shell getprop ro.build.display.id") echo "SQ3A.220605.009.A1.TEST" ;;
  "shell getprop ro.build.version.release") echo "12" ;;
  "shell dumpsys package com.readyatdawn.r15")
    printf '  Package [com.readyatdawn.r15]\n    versionCode=4987566 minSdk=29 targetSdk=29\n    versionName=4987566\n    installerPackageName=%s\n' "$FAKE_EMAIL" ;;
  "shell cat "*"/.echo_installer_version")
    if [[ -n "${FAKE_NO_INSTALLER:-}" ]]; then echo "cat: ${2#cat }: No such file or directory"; exit 1; fi
    echo "echo-installer 2.0.1" ;;
  "shell ls -la "*)
    dir=${2#ls -la }
    if has_dir "$dir"; then printf 'total 8\ndrwxrwx--- 2 u0_a1 media_rw 4096 2026-10-21 22:00 .\n-rw-rw---- 1 u0_a1 media_rw 12 2026-10-21 22:00 device_login.txt\n'; else echo "ls: $dir: No such file or directory"; exit 1; fi ;;
  "shell [ -d "*) dir=${2#\[ -d }; dir=${dir%% *}; if has_dir "$dir"; then echo yes; else echo no; fi ;;
  "shell am force-stop "*) ;;
  "logcat -c") ;;
  "shell am start -n "*) start_default="Starting: Intent { cmp=$5 }"; printf '%s\n' "${FAKE_START_OUT:-$start_default}" ;;
  "shell pidof "*) if [[ -n "${FAKE_NOT_RUNNING:-}" ]]; then exit 1; fi; echo 4242 ;;
  "logcat -d -b all") cat "$FAKE_LOGCAT" ;;
  *) echo "unexpected adb call: $*" >&2; exit 9 ;;
esac
"""

NEEDED_TOOLS = ("bash", "env", "sh", "awk", "sed", "grep", "tr", "head", "cut", "cat", "dirname", "basename", "mktemp",
                "date", "mv", "rm", "mkdir", "find", "sleep", "timeout", "zip", "python3", "uname", "ls", "cp", "chmod")


def tool_farm(directory: pathlib.Path) -> None:
    """A PATH with the tools the scripts use and no adb, so the real adb on this machine is never found."""
    directory.mkdir()
    for name in NEEDED_TOOLS:
        found = shutil.which(name)
        if found:
            (directory / name).symlink_to(found)


class DiagnoseCases:
    """The behaviour both scripts share. A subclass says how to run its script."""

    kind = ""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="quest-diagnose-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.scripts = self.tmp / "scripts"
        self.scripts.mkdir()
        shutil.copy(self.script_source, self.scripts / self.script_source.name)
        self.script = self.scripts / self.script_source.name
        self.farm = self.tmp / "farm"
        tool_farm(self.farm)
        self.fakebin = self.tmp / "fakebin"
        self.fakebin.mkdir()
        self.adb = self.fakebin / "adb"
        self.adb.write_text(FAKE_ADB)
        self.adb.chmod(0o755)
        self.logcat = self.tmp / "logcat.txt"
        self.logcat.write_text(LOGCAT)
        self.adb_log = self.tmp / "adb.log"
        self.out = self.tmp / "out"
        self.out.mkdir()

    def env(self, **extra):
        env = {"PATH": f"{self.fakebin}:{self.farm}", "HOME": str(self.tmp / "home"), "TMPDIR": str(self.tmp),
               "APPDATA": str(self.tmp / "Roaming"), "LOCALAPPDATA": str(self.tmp / "Local"),
               "FAKE_ADB_LOG": str(self.adb_log), "FAKE_DIRS": GOOD_DIRS, "FAKE_LOGCAT": str(self.logcat),
               "FAKE_SERIAL": SERIAL, "FAKE_EMAIL": EMAIL, "FAKE_HWSERIAL": HWSERIAL}
        env.update(extra)
        return env

    def calls(self):
        return self.adb_log.read_text().splitlines() if self.adb_log.exists() else []

    def zips(self):
        return sorted(self.out.glob("nevr-diagnose-*.zip"))

    def members(self):
        (path,) = self.zips()
        with zipfile.ZipFile(path) as z:
            return {name: z.read(name) for name in z.namelist()}, [i.filename for i in z.infolist()]

    def test_the_zip_holds_every_listed_file_with_the_verdict_first(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data, order = self.members()
        self.assertEqual(order[0], "verdict.txt")
        for name in ("device.txt", "package.txt", "installer_version.txt", "launch.txt", "logcat.txt",
                     "listings/readyatdawn.txt", "listings/readyatdawn_data.txt", "listings/media.txt",
                     "listings/media_files.txt", "listings/media_files_data.txt", "listings/media_asset_patches.txt",
                     "listings/android_data_files.txt", "sentinel/nevr-sentinel.log",
                     "sentinel/nevr-crashes/crash-1.txt", "sentinel/nevr-crashes/crash-1.dmp"):
            self.assertIn(name, data, f"{name} missing from {sorted(data)}")
        self.assertIn(b"Quest 2", data["device.txt"])
        self.assertIn(b"SQ3A.220605.009.A1.TEST", data["device.txt"])
        self.assertIn(b"versionCode: 4987566", data["package.txt"])
        self.assertIn(b"versionName: 4987566", data["package.txt"])
        self.assertIn(b"echo-installer 2.0.1", data["installer_version.txt"])
        self.assertTrue(re.fullmatch(r"nevr-diagnose-\d{8}T\d{6}Z\.zip", self.zips()[0].name), self.zips()[0].name)

    def test_the_zip_holds_exactly_the_listed_files(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data, _ = self.members()
        self.assertEqual(set(data), ZIP_FILES)

    def test_the_verdict_prints_first_on_one_screen_and_says_where_the_data_was_found(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        start = result.stdout.index("NEVR Quest diagnose")
        verdict = result.stdout[start:].split("Privacy:")[0].strip().splitlines()
        self.assertLessEqual(len(verdict), 16, verdict)
        self.assertFalse([line for line in verdict if line.startswith("Zip:")][0].count("/"), verdict)
        text = "\n".join(verdict)
        self.assertIn("Headset:   Quest 2, OS build SQ3A.220605.009.A1.TEST", text)
        self.assertIn("versionCode 4987566, versionName 4987566", text)
        self.assertIn("Installer: echo-installer 2.0.1", text)
        self.assertRegex(text, rf"{re.escape(MEDIA)}/files/_data: present")
        self.assertRegex(text, rf"{re.escape(LEGACY)}/_data: absent")
        self.assertIn(f"the game mounted data at: {MEDIA}/files", text)
        self.assertIn("runtime data_root line: result=found source=android_media", text)
        self.assertIn("running after 0 s: yes", text)
        self.assertIn("No problem found in these facts", text)
        self.assertNotIn("Traceback", result.stderr)
        self.assertEqual(self.members()[0]["verdict.txt"].decode().strip(), text)

    def test_the_serial_and_the_email_are_nowhere(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data, _ = self.members()
        everything = [result.stdout, result.stderr, self.zips()[0].name, " ".join(data)]
        everything += [body.decode("utf-8", "replace") for body in data.values()]
        log = self.scripts / "nevr-diagnose.log"
        if log.exists():
            everything.append(log.read_text())
        for text in everything:
            self.assertNotIn(SERIAL, text)
            self.assertNotIn(HWSERIAL, text)
            self.assertNotIn(EMAIL, text)
        self.assertIn(b"<device-serial>", data["logcat.txt"])
        self.assertIn(b"<email>", data["logcat.txt"])
        self.assertIn(b"<device-serial>", data["sentinel/nevr-sentinel.log"])
        self.assertIn(b"<email>", data["sentinel/nevr-sentinel.log"])
        self.assertIn(b"<email>", data["package.txt"] + data["sentinel/nevr-sentinel.log"])

    def test_the_hardware_serial_is_masked_when_the_headset_is_connected_over_wifi(self):
        result = self.run_script(FAKE_DEVICES=f"{WIFI_SERIAL}\\tdevice\\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data, _ = self.members()
        everything = [result.stdout, result.stderr] + [body.decode("utf-8", "replace") for body in data.values()]
        for text in everything:
            self.assertNotIn(HWSERIAL, text)
            self.assertNotIn(WIFI_SERIAL, text)
        self.assertIn(b"<device-serial>", data["logcat.txt"])
        self.assertIn(b"<device-serial>", data["logcat_before_launch.txt"])
        self.assertIn(b"<device-serial>", data["sentinel/nevr-crashes/crash-1.txt"])
        # What is not masked stays, and the zip says so (the privacy line).
        self.assertIn(b"bssid=aa:bb:cc:dd:ee:ff", data["logcat.txt"])

    def test_a_serial_with_characters_that_mean_something_in_a_pattern_is_still_masked(self):
        odd = "Q+1(a)|b.c?"
        self.logcat.write_text(LOGCAT + f"10-21 22:00:04.000  100  100 I X: serial={odd} again\n")
        result = self.run_script(FAKE_DEVICES=f"{odd}\\tdevice\\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data, _ = self.members()
        for text in [result.stdout, result.stderr] + [body.decode("utf-8", "replace") for body in data.values()]:
            self.assertNotIn(odd, text)
        self.assertIn(b"serial=<device-serial> again", data["logcat.txt"])

    def test_the_last_line_says_what_the_zip_holds_and_where_to_send_it(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        last = [line for line in result.stdout.splitlines() if line.strip()][-1]
        for needle in ("crash dumps", "system log", "Wi-Fi name", "BSSID", "IP addresses", "numeric user ids", CONTACT, PRIVACY):
            self.assertIn(needle, last)

    def test_the_log_of_the_run_the_tester_came_with_is_saved_before_it_is_erased(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls()
        cleared = calls.index("logcat -c")
        self.assertIn("logcat -d -b all", calls[:cleared])
        self.assertIn("logcat -d -b all", calls[cleared:])
        data, _ = self.members()
        self.assertIn(b"OBB mounted at", data["logcat_before_launch.txt"])

    def test_the_zip_line_names_the_file_not_the_folder(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        zip_line = [line for line in result.stdout.splitlines() if line.startswith("Zip:")]
        self.assertEqual(len(zip_line), 1, result.stdout)
        self.assertIn(self.zips()[0].name, zip_line[0])
        self.assertNotIn(str(self.out), zip_line[0])
        self.assertNotIn(str(self.out), self.members()[0]["verdict.txt"].decode())

    def test_a_relative_output_folder_is_resolved_before_the_work_folder_is_entered(self):
        cwd = self.tmp / "cwd"
        (cwd / "rel out").mkdir(parents=True)
        for out, folder in (("rel out", cwd / "rel out"), (".", cwd)):
            result = self.run_script(out=out, cwd=cwd)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            made = sorted(folder.glob("nevr-diagnose-*.zip"))
            self.assertEqual(len(made), 1, f"--out {out!r}: {sorted(p.name for p in folder.iterdir())}")
            with zipfile.ZipFile(made[0]) as z:
                self.assertEqual(z.namelist()[0], "verdict.txt")

    def test_the_files_folder_alone_is_data_the_runtime_serves_and_the_verdict_says_so(self):
        result = self.run_script(FAKE_DIRS=f"{MEDIA}:{MEDIA}/files:{FILES}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("NO GAME DATA", result.stdout)
        self.assertRegex(result.stdout, rf"{re.escape(MEDIA)}/files/_data: absent")
        self.assertIn("android_media_unmarked", result.stdout)

    def test_device_login_txt_is_never_collected(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse([c for c in self.calls() if "device_login" in c], self.calls())
        data, _ = self.members()
        self.assertFalse([n for n in data if "device_login" in n], sorted(data))
        pulls = [c for c in self.calls() if c.startswith("pull ")]
        # The runtime's folder under Android/media is read first; the Android/data folder is not touched when it answers.
        self.assertEqual(sorted(p.split()[1] for p in pulls),
                         [f"{MEDIA}/files/nevr-crashes", f"{MEDIA}/files/nevr-sentinel.log",
                          f"{MEDIA}/files/nevr-sentinel.session-1.log"])
        data, _ = self.members()
        self.assertIn("sentinel/nevr-sentinel.session-1.log", data)
        self.assertIn(b"<email>", data["sentinel/nevr-sentinel.session-1.log"])

    def test_the_android_data_folder_is_the_fallback_for_the_log_and_the_crashes(self):
        result = self.run_script(FAKE_NO_MEDIA_LOGS="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        pulls = [c.split()[1] for c in self.calls() if c.startswith("pull ")]
        self.assertEqual(pulls[:2], [f"{MEDIA}/files/nevr-sentinel.log", f"{FILES}/nevr-sentinel.log"])
        self.assertIn(f"{FILES}/nevr-crashes", pulls)
        self.assertIn(f"{FILES}/nevr-sentinel.session-1.log", pulls)
        data, _ = self.members()
        self.assertIn("sentinel/nevr-sentinel.log", data)
        self.assertIn("sentinel/nevr-crashes/crash-1.txt", data)

    def test_the_launch_is_one_clean_launch_in_order(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls()
        wanted = ["shell am force-stop com.readyatdawn.r15", "logcat -c",
                  "shell am start -n com.readyatdawn.r15/com.oculus.gles3jni.MainActivity",
                  "shell pidof com.readyatdawn.r15", "logcat -d -b all"]
        position = -1
        for needle in wanted:
            found = [i for i, c in enumerate(calls) if c == needle and i > position]
            self.assertTrue(found, f"{needle!r} missing or out of order in {calls}")
            position = found[0]
        self.assertEqual(len([c for c in calls if c.startswith("shell am start")]), 1)

    def test_nothing_on_the_headset_is_changed(self):
        self.run_script()
        for call in self.calls():
            self.assertNotRegex(call, r"^(install|uninstall|push|root|remount|reboot)\b", call)
            self.assertNotRegex(call, r"\b(rm|mv|cp|pm clear|pm uninstall|rmdir|mkdir)\b", call)

    def test_no_data_anywhere_is_in_the_verdict(self):
        result = self.run_script(FAKE_DIRS=FILES)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NO GAME DATA", result.stdout)
        self.assertRegex(result.stdout, rf"{re.escape(MEDIA)}/files/_data: absent")

    def test_the_legacy_folder_alone_counts_as_data(self):
        result = self.run_script(FAKE_DIRS=f"{LEGACY}:{LEGACY}/_data:{FILES}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("NO GAME DATA", result.stdout)
        self.assertRegex(result.stdout, rf"{re.escape(LEGACY)}/_data: present")

    def test_a_game_that_is_not_running_is_in_the_verdict(self):
        result = self.run_script(FAKE_NOT_RUNNING="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("was not running", result.stdout)
        self.assertIn("running after 0 s: no", result.stdout)

    def test_a_crash_marker_is_in_the_verdict_and_a_missing_data_line_is_not_a_failure(self):
        self.logcat.write_text("10-21 22:00:01.000 100 100 E AndroidRuntime: FATAL EXCEPTION: main\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("crash marker: FATAL EXCEPTION", result.stdout)
        self.assertNotIn("data step", result.stdout)
        self.assertIn("none (this build does not log one)", result.stdout)

    # #764: the verdict of the first real-headset run called a healthy launch failed because that build logs no
    # "OBB mounted at" line. A game still running after the wait is never "failed" for a line that is absent.
    HEALTHY_WITHOUT_THE_MOUNT_LINE = (
        "10-22 00:02:31.000  100  100 I NEVR-Sentinel: {\"event\":\"sentinel_ctor\"}\n"
        "10-22 00:02:33.000  100  100 I RAD: Starting 2 worker threads in the scheduler.\n"
    )

    def test_a_running_game_whose_build_logs_no_data_mount_line_is_not_called_failed(self):
        self.logcat.write_text(self.HEALTHY_WITHOUT_THE_MOUNT_LINE)
        result = self.run_script(FAKE_DIRS=f"{LEGACY}:{LEGACY}/_data:{FILES}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = result.stdout.split("Privacy:")[0]
        self.assertNotIn("did not reach its data step", text)
        self.assertNotIn("NO GAME DATA", text)
        self.assertIn("running after 0 s: yes", text)
        self.assertIn("No failure seen: running after 0 s", text)
        self.assertIn("this build logged no data-mount line, so the data step can't be confirmed from the log", text)

    # #834: NO GAME DATA is judged from the folders alone. A running game whose log has neither an OBB mount line
    # nor a data_root event, with no files folder and no _data folder, is still flagged.
    def test_a_running_game_with_no_data_folders_and_no_mount_or_data_root_line_is_flagged_no_game_data(self):
        self.logcat.write_text(self.HEALTHY_WITHOUT_THE_MOUNT_LINE)
        result = self.run_script(FAKE_DIRS=FILES)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = result.stdout.split("Privacy:")[0]
        self.assertIn("running after 0 s: yes", text)
        self.assertIn("the game mounted data at: (no line)", text)
        self.assertIn("runtime data_root line: none (this build does not log one)", text)
        self.assertIn("NO GAME DATA", text)
        self.assertNotIn("No failure seen", text)

    def test_a_runtime_data_root_line_confirms_the_data_step_without_an_obb_line(self):
        self.logcat.write_text(
            f'10-22 00:02:33.000  100  100 I NEVR-Sentinel: {{"event":"data_root","result":"found","source":"legacy","root":"{LEGACY}"}}\n')
        result = self.run_script()
        text = result.stdout.split("Privacy:")[0]
        self.assertIn("runtime data_root line: result=found source=legacy", text)
        self.assertIn("No problem found in these facts", text)
        self.assertNotIn("can't be confirmed", text)

    def test_an_initialization_error_is_a_failure_and_names_the_file_the_game_wanted(self):
        wanted = "/storage/emulated/0/readyatdawn/_data/5932408047/rad15/android/manifests/48037dc70b0ecab2"
        self.logcat.write_text(
            "10-22 00:00:08.581  7717  7717 E RAD: Initialization Error: Unknown error while loading the game.\n"
            f"10-22 00:00:08.590  7717  7717 E RAD: - 0x2FDA8A8C4D75508F: Failed to find package manifest file {wanted}\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = result.stdout.split("Privacy:")[0]
        self.assertIn("The game logged: Initialization Error: Unknown error while loading the game.", text)
        self.assertIn(f"Failed to find package manifest file {wanted}", text)
        self.assertNotIn("No failure seen", text)
        self.assertNotIn("No problem found", text)

    def test_an_initialization_error_from_a_windows_adb_keeps_no_carriage_return(self):
        self.logcat.write_bytes(
            b"10-22 00:00:08.581  7717  7717 E RAD: Initialization Error: Unknown error while loading the game.\r\n"
            b"10-22 00:00:08.590  7717  7717 E RAD: - 0x2FDA8A8C4D75508F: Failed to find package manifest file /x/y\r\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        verdict = self.members()[0]["verdict.txt"]
        self.assertIn(b"The game logged: Initialization Error: Unknown error while loading the game. (", verdict)
        self.assertNotIn(b"\r", verdict)

    def test_an_obb_mounted_line_alone_confirms_the_data_step(self):
        self.logcat.write_text(f"10-21 22:00:03.100  100  100 I EchoVR : OBB mounted at '{MEDIA}/files'\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = result.stdout.split("Privacy:")[0]
        self.assertIn("No problem found in these facts", text)
        self.assertNotIn("can't be confirmed", text)

    def test_a_game_that_is_not_running_is_the_failure_not_a_missing_data_line(self):
        self.logcat.write_text(self.HEALTHY_WITHOUT_THE_MOUNT_LINE)
        result = self.run_script(FAKE_NOT_RUNNING="1")
        text = result.stdout.split("Privacy:")[0]
        self.assertIn("was not running", text)
        self.assertNotIn("data step", text)
        self.assertNotIn("No failure seen", text)

    def test_an_am_start_error_is_in_the_verdict(self):
        result = self.run_script(FAKE_START_OUT="Error: Activity class {x/y} does not exist.")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("The game did not start: Error: Activity class", result.stdout)

    def test_missing_files_on_the_headset_are_recorded_not_fatal(self):
        result = self.run_script(FAKE_NO_SENTINEL_LOG="1", FAKE_NO_INSTALLER="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Installer: no .echo_installer_version found", result.stdout)
        data, _ = self.members()
        self.assertIn(b"exit 1", data["sentinel/pull_results.txt"])
        self.assertNotIn("sentinel/nevr-sentinel.log", data)

    def test_no_device_two_devices_and_an_unauthorized_one_are_named_before_anything_runs(self):
        for devices, needle in (("", "No headset is connected"),
                                (f"{SERIAL}\\tdevice\\nOTHER99\\tdevice\\n", "2 devices are connected"),
                                (f"{SERIAL}\\tunauthorized\\n", "is unauthorized")):
            self.adb_log.unlink(missing_ok=True)
            result = self.run_script(FAKE_DEVICES=devices)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(needle, result.stdout + result.stderr)
            self.assertNotIn(SERIAL, result.stdout + result.stderr)
            self.assertEqual(self.calls(), [])
            self.assertEqual(self.zips(), [])
        log = (self.scripts / "nevr-diagnose.log").read_text()
        self.assertIn("ERROR", log)
        self.assertNotIn(SERIAL, log)

    def test_adb_found_nowhere_names_where_it_looked(self):
        result = self.run_script(PATH=str(self.farm))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("adb was not found", result.stdout + result.stderr)
        self.assertEqual(self.calls(), [])

    def test_an_explicit_adb_beats_the_path(self):
        other = self.tmp / "explicit-adb"
        other.write_text(FAKE_ADB.replace("Quest 2", "Quest Explicit"))
        other.chmod(0o755)
        result = self.run_script(PATH=str(self.farm), adb=str(other))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Quest Explicit", result.stdout)
        self.assertIn(f"Using adb: {other}", result.stdout)


class SourceShape(unittest.TestCase):
    """What a run against a fake adb cannot show."""

    def test_the_scripts_delete_only_the_working_folder_they_made(self):
        ps1 = PS1.read_text()
        self.assertEqual(len(re.findall(r"Remove-Item", ps1)), 1)
        self.assertIn("Remove-Item -LiteralPath $script:Work -Recurse -Force", ps1)
        self.assertNotRegex(ps1, r"\bdel\b|\brm\b")
        sh = SH.read_text()
        self.assertEqual(re.findall(r"\brm\b[^\n]*", re.sub(r"^\s*#.*$", "", sh, flags=re.M)),
                         ['rm -rf "$work"\' EXIT'])

    def test_no_adb_command_names_the_sign_in_file_or_changes_the_headset(self):
        for path in (PS1, SH):
            code = re.sub(r"^\s*#.*$", "", path.read_text(), flags=re.M)
            self.assertNotIn("device_login", code, path.name)
            self.assertNotRegex(code, r"adb[^\n]*\b(install|uninstall|push|rm|clear)\b", path.name)


    def test_the_scripts_and_the_readme_say_the_same_things_about_privacy_and_what_changes(self):
        readme = (REPO / "tools/package-release/README.template.txt").read_text()
        for path in (PS1, SH):
            header = " ".join(re.sub(r"^#\s?", "", line) for line in path.read_text().splitlines()[:25] if line.startswith("#"))
            header = re.sub(r"\s+", " ", header)
            for needle in ("crash dumps", "Wi-Fi name", "BSSID", "IP addresses", "numeric user ids", CONTACT, PRIVACY,
                           "ro.serialno", "erases the headset's log buffer", "logcat_before_launch.txt"):
                self.assertIn(needle, header, f"{path.name}: {needle}")
            self.assertNotIn("changes nothing", header)
        flat = re.sub(r"\s+", " ", readme)
        for needle in ("crash dumps", "Wi-Fi name", "BSSID", "IP addresses", "numeric user ids", CONTACT, PRIVACY,
                       "serial numbers", "erases the headset's log buffer"):
            self.assertIn(needle, flat, needle)
        self.assertNotIn("changes nothing", flat)
        self.assertIn("Needs bash 4", SH.read_text())


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class QuestDiagnosePs1Test(DiagnoseCases, unittest.TestCase):
    script_source = PS1

    def run_script(self, adb=None, pause=False, stdin="\n", out=None, cwd=None, **extra):
        cmd = [shutil.which("pwsh"), "-NoProfile", "-File", str(self.script), "-OutDir", out or str(self.out),
               "-LaunchWaitSeconds", "0"]
        if adb:
            cmd += ["-Adb", adb]
        cmd.append("-Pause" if pause else "-NoPause")
        return subprocess.run(cmd, env=self.env(**extra), input=stdin, capture_output=True, text=True, timeout=120,
                              cwd=cwd)

    def test_adb_is_found_in_sidequests_folder_after_the_path(self):
        platform_tools = self.tmp / "Roaming" / "SideQuest" / "platform-tools"
        platform_tools.mkdir(parents=True)
        shutil.copy(self.adb, platform_tools / "adb.exe")
        result = self.run_script(PATH=str(self.farm))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"Using adb: {platform_tools / 'adb.exe'}", result.stdout)

    def test_the_window_waits_for_enter_with_pause_and_never_with_nopause(self):
        waits = self.run_script(pause=True)
        self.assertEqual(waits.returncode, 0, waits.stdout + waits.stderr)
        self.assertIn("Press Enter to close", waits.stdout)
        failed = self.run_script(pause=True, FAKE_DEVICES="")
        self.assertEqual(failed.returncode, 1)
        self.assertIn("Press Enter to close", failed.stdout)
        quiet = self.run_script()
        self.assertNotIn("Press Enter", quiet.stdout)


@unittest.skipUnless(shutil.which("bash") and shutil.which("zip"), "bash and zip not installed")
class QuestDiagnoseShTest(DiagnoseCases, unittest.TestCase):
    script_source = SH

    def run_script(self, adb=None, out=None, cwd=None, **extra):
        cmd = [shutil.which("bash"), str(self.script), "--out", out or str(self.out), "--wait", "0"]
        if adb:
            cmd += ["--adb", adb]
        return subprocess.run(cmd, env=self.env(**extra), input="", capture_output=True, text=True, timeout=120,
                              cwd=cwd)

    def test_wait_without_a_value_fails_fast_with_a_plain_message(self):
        result = subprocess.run([shutil.which("bash"), str(self.script), "--wait"], env=self.env(), input="",
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("--wait needs a number of seconds", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_the_zip_is_written_without_the_zip_tool_when_python3_exists(self):
        no_zip = self.tmp / "no-zip-farm"
        tool_farm(no_zip)
        (no_zip / "zip").unlink()
        result = self.run_script(PATH=f"{self.fakebin}:{no_zip}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.members()[1][0], "verdict.txt")

    def test_without_zip_and_python3_the_error_is_plain(self):
        bare = self.tmp / "bare-farm"
        tool_farm(bare)
        (bare / "zip").unlink()
        (bare / "python3").unlink()
        result = self.run_script(PATH=f"{self.fakebin}:{bare}")
        self.assertEqual(result.returncode, 1)
        self.assertIn("neither zip nor python3 is installed", result.stderr)


if __name__ == "__main__":
    unittest.main()
