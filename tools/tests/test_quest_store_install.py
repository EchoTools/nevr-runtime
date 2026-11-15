"""tools/quest_store_install.py (docs/adr/0042): the headset step, run against a fake `adb` on PATH.

The fake is a script that keeps a device's /sdcard in a scratch directory and records every call it gets, so the tests can say
which calls were made and which were not. No real device is touched: a real headset is the owner's and claude-main's to run.

The removal rule (ORDER.md "The manifest rule"): the tool removes on the headset only the files its build manifest lists, or the
record the previous install left (`_data.files`), each by its literal path; a folder goes only when empty. Nothing is removed with
`rm -rf`, and a folder that still holds an unlisted file is moved aside to `_data.unrecorded`. The fake refuses `rm -rf` outright,
so a tool that issues one fails the test that runs it.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "tests"))
import quest_store_fixtures as fx  # noqa: E402

qsb = fx.qsb
TOOL = REPO / "tools" / "quest_store_install.py"


def run_logged(fn, *args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        result = fn(*args)
    return result, out.getvalue()

DEVICE_DATA = "sdcard/Android/media/com.readyatdawn.r15/files/_data"
DEVICE_FILES = "sdcard/Android/media/com.readyatdawn.r15/files"
RECORD = DEVICE_FILES + "/_data.files"

FAKE_ADB = textwrap.dedent('''\
    #!%(python)s
    import hashlib, json, os, pathlib, shlex, shutil, sys
    root = pathlib.Path(os.environ["FAKE_ADB_ROOT"])
    args = sys.argv[1:]
    with open(root / "calls.log", "a") as f:
        f.write(json.dumps(args) + "\\n")
    if args[:1] == ["-s"]:
        args = args[2:]
    def dev(path):
        return root / "device" / path.lstrip("/")
    cmd = args[0]
    if cmd == "devices":
        print("List of devices attached")
        for item in filter(None, os.environ.get("FAKE_ADB_DEVICES", "").split(",")):
            print(item.replace(":", "\\t"))
    elif cmd == "install":
        if os.environ.get("FAKE_ADB_INSTALL") == "fail":
            print("Performing Streamed Install")
            print("adb: failed to install x.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: signatures do not match]")
            sys.exit(1)
        if os.environ.get("FAKE_ADB_INSTALL") == "softfail":
            print("Failure [INSTALL_FAILED_VERSION_DOWNGRADE]")  # some adb versions exit 0 on this
            sys.exit(0)
        (root / "installed.log").write_text(args[-1])
        print("Performing Streamed Install")
        print("Success")
        if os.environ.get("FAKE_ADB_INSTALL") == "rc":
            sys.exit(1)  # says Success and still exits non-zero: the exit code decides
    elif cmd == "push":
        if os.environ.get("FAKE_ADB_PUSH") == "fail":
            print("adb: error: failed to copy: No space left on device", file=sys.stderr)
            sys.exit(1)
        local, remote = pathlib.Path(args[1]), dev(args[2])
        if local.is_dir():
            target = remote / local.name if remote.is_dir() else remote
            shutil.copytree(local, target)
            if os.environ.get("FAKE_ADB_CORRUPT"):
                victim = sorted(p for p in target.rglob("*") if p.is_file())[0]
                victim.write_bytes(victim.read_bytes()[:-1] + b"!")
            print("%%d files pushed" %% sum(1 for p in target.rglob("*") if p.is_file()))
        else:
            target = remote / local.name if remote.is_dir() else remote
            shutil.copyfile(local, target)
            print("1 file pushed")
    elif cmd == "shell":
        sh = shlex.split(" ".join(args[1:]))  # the device's shell reads the one command line adb joins
        if sh[:1] == ["rm"]:
            if any(a.startswith("-") and "r" in a[1:] for a in sh[1:]):
                print("fake adb: recursive rm is refused in this fake", file=sys.stderr)
                sys.exit(97)
            p = dev(sh[-1])
            if p.is_dir() and not p.is_symlink():
                print("rm: " + sh[-1] + ": is a directory", file=sys.stderr)
                sys.exit(1)
            if p.exists() or p.is_symlink():
                p.unlink()
            sys.exit(0)  # rm -f: a missing file is not an error
        elif sh[:1] == ["ls"]:
            p = dev(sh[-1])
            if not p.is_dir():
                print("ls: " + sh[-1] + ": No such file or directory", file=sys.stderr)
                sys.exit(1)
            print("\\n".join(sorted(c.name for c in p.iterdir())))
        elif sh[:1] == ["rmdir"]:
            try:
                dev(sh[-1]).rmdir()
            except OSError as error:
                print("rmdir: " + sh[-1] + ": " + error.strerror, file=sys.stderr)
                sys.exit(1)
        elif sh[:1] == ["test"]:
            p = dev(sh[2])
            sys.exit(0 if (p.is_dir() if sh[1] == "-d" else p.exists()) else 1)
        elif sh[:1] == ["cat"]:
            p = dev(sh[-1])
            if not p.is_file():
                print("cat: " + sh[-1] + ": No such file or directory", file=sys.stderr)
                sys.exit(1)
            sys.stdout.write(p.read_text())
        elif sh[:2] == ["mkdir", "-p"]:
            dev(sh[2]).mkdir(parents=True, exist_ok=True)
        elif sh[0] == "mv":
            dev(sh[2]).parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(dev(sh[1])), str(dev(sh[2])))
            if os.environ.get("FAKE_ADB_LOSE") and dev(sh[2]).is_dir():
                shutil.rmtree(dev(sh[2]))  # the move of the data "succeeds" and the data is not there
        elif sh[0] == "sha256sum":
            p = dev(sh[1])
            if not p.is_file():
                print("sha256sum: " + sh[1] + ": No such file or directory", file=sys.stderr)
                sys.exit(1)
            print(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + sh[1])
        elif sh[:2] == ["stat", "-c"]:
            p = dev(sh[3])
            if not p.is_file():
                sys.exit(1)
            print(p.stat().st_size)
        else:
            print("fake adb: unknown shell command " + " ".join(sh), file=sys.stderr)
            sys.exit(127)
    else:
        print("fake adb: unknown command " + cmd, file=sys.stderr)
        sys.exit(1)
''' % {"python": sys.executable})


class StoreInstallTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = pathlib.Path(self.tmp.name)
        self.layout, self.apk = fx.make_root(self.base)
        status, _ = run_logged(qsb.collect, self.layout, self.apk, fx.RECIPE, fx.SIGNER)
        self.assertEqual(status, 0)
        self.fake = self.base / "fake"
        (self.fake / "bin").mkdir(parents=True)
        adb = self.fake / "bin" / "adb"
        adb.write_text(FAKE_ADB)
        adb.chmod(0o755)
        old = self.fake / "device" / DEVICE_DATA
        old.mkdir(parents=True)
        (old / "old_stock.bin").write_bytes(b"data from an earlier install")

    def run_tool(self, *args, devices="SERIAL1:device", with_adb=True, **env):
        path = str(self.fake / "bin") if with_adb else str(self.base / "empty")
        environment = {"PATH": path, "FAKE_ADB_ROOT": str(self.fake), "FAKE_ADB_DEVICES": devices, **env}
        done = subprocess.run([sys.executable, "-I", str(TOOL), "--root", str(self.layout.root), *args], capture_output=True, text=True, env=environment)
        lines = [json.loads(line) for line in done.stdout.splitlines()]
        for line in lines:
            self.assertEqual((line["tag"], line["schema"]), ("NEVR.QUESTBUILD", 1))
        return done, lines

    def calls(self):
        log = self.fake / "calls.log"
        calls = [json.loads(l) for l in log.read_text().splitlines()] if log.exists() else []
        return [c[2:] if c[:1] == ["-s"] else c for c in calls]

    def shell_calls(self):
        return [c[1:] for c in self.calls() if c[:1] == ["shell"]]

    def device_files(self):
        base = self.fake / "device" / DEVICE_DATA
        return {p.relative_to(base).as_posix(): p.read_bytes() for p in base.rglob("*") if p.is_file()} if base.exists() else {}

    def device_path(self, rel):
        return self.fake / "device" / rel

    def aside_dirs(self):
        """The folders set aside under _data.unrecorded/<stamp>/ on the device."""
        root = self.fake / "device" / DEVICE_FILES / "_data.unrecorded"
        return sorted(p for p in root.iterdir() if p.is_dir()) if root.exists() else []

    def refused(self, lines):
        self.assertEqual(lines[-1]["event"], "refused")
        return lines[-1]["code"]

    def test_the_install_replaces_the_data_and_logs_each_adb_call(self):
        done, lines = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        stock = {p[len("_data/"):]: c for p, c in fx.DATA.items()}
        self.assertEqual(self.device_files(), stock)  # the stock tree, and nothing of the old one in _data
        # the old install's file is not listed by any record, so its folder is moved aside under a new name, never deleted
        asides = self.aside_dirs()
        self.assertEqual(len(asides), 1, asides)
        self.assertEqual((asides[0] / "old_stock.bin").read_bytes(), b"data from an earlier install")
        self.assertFalse((self.fake / "device" / "sdcard/Android/media/com.readyatdawn.r15/files/.nevr-stock-staging").exists())
        self.assertEqual((self.fake / "installed.log").read_text(), str(self.layout.out / qsb.SIGNED_APK_NAME))
        events = [l["event"] for l in lines]
        self.assertEqual(events[:3], ["store_build_read", "adb_devices", "device_chosen"])
        self.assertEqual(events[-1], "store_install_done")
        for needed in ("apk_install", "apk_installed", "data_push", "device_data_verified", "data_replaced"):
            self.assertIn(needed, events)
        self.assertEqual(events.count("device_hash"), len(fx.DATA))
        for line in lines:
            if line["event"] in ("apk_install", "data_push", "device_hash"):
                self.assertEqual(line["argv"][0], "adb")
                self.assertIn("exit_code", line)
        order = [c[0] if c[0] != "shell" else "shell:" + c[1] for c in self.calls()]
        self.assertLess(order.index("install"), order.index("push"))
        self.assertLess(order.index("push"), order.index("shell:sha256sum"))
        self.assertLess(order.index("shell:sha256sum"), order.index("shell:mv"))

    def test_no_recursive_removal_is_ever_issued(self):
        done, _ = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        for call in self.shell_calls():
            self.assertFalse(call[:1] == ["rm"] and any(a.startswith("-") and "r" in a[1:] for a in call[1:]), call)
        self.assertTrue(any(c[:2] == ["rm", "-f"] for c in self.shell_calls()))  # the removal is by named file

    def test_the_record_of_what_was_pushed_is_left_beside_the_data(self):
        done, _ = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        record = self.device_path(RECORD).read_text().splitlines()
        self.assertEqual(record, sorted(p for p in fx.DATA))  # every path the install pushed, one per line

    def test_a_recorded_file_is_removed_by_its_name_and_the_unlisted_one_moves_the_folder_aside(self):
        self.device_path(RECORD).write_text("_data/recorded.bin\n")
        (self.fake / "device" / DEVICE_DATA / "recorded.bin").write_bytes(b"recorded by the last install")
        done, _ = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn(["rm", "-f", "--", "/" + DEVICE_DATA + "/recorded.bin"], [c for c in self.shell_calls()])
        asides = self.aside_dirs()
        self.assertEqual(len(asides), 1)
        self.assertFalse((asides[0] / "recorded.bin").exists())
        self.assertEqual((asides[0] / "old_stock.bin").read_bytes(), b"data from an earlier install")

    def test_an_earlier_aside_is_left_alone_and_the_data_is_moved_aside_under_a_new_name(self):
        earlier = self.device_path(DEVICE_FILES + "/_data.unrecorded/20260101T000000000000Z-0000")
        earlier.mkdir(parents=True)
        (earlier / "kept.bin").write_bytes(b"from an earlier set-aside")
        done, lines = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual((earlier / "kept.bin").read_bytes(), b"from an earlier set-aside")
        self.assertEqual(len(self.aside_dirs()), 2)  # the old one and the new one

    def test_a_recorded_removal_that_empties_the_data_moves_nothing_aside(self):
        self.device_path(RECORD).write_text("_data/old_stock.bin\n")
        done, lines = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(self.aside_dirs(), [], "the recorded removal emptied _data, so nothing is set aside")
        stock = {p[len("_data/"):]: c for p, c in fx.DATA.items()}
        self.assertEqual(self.device_files(), stock)

    def test_a_fourth_set_aside_succeeds_and_the_warning_names_four(self):
        for stamp in ("1", "2", "3"):
            self.device_path(DEVICE_FILES + f"/_data.unrecorded/2026010{stamp}T000000000000Z-0000").mkdir(parents=True)
        done, lines = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(len(self.aside_dirs()), 4, "the new set-aside is one more folder under _data.unrecorded")
        warnings = [l for l in lines if l["event"] == "old_copies_kept"]
        self.assertEqual(len(warnings), 1, lines)
        self.assertEqual(warnings[0]["level"], "warning")
        self.assertEqual(warnings[0]["count"], 4)
        self.assertIn("remove them to free space", warnings[0]["detail"])

    def test_a_record_entry_that_is_now_a_folder_is_skipped_with_a_warning(self):
        self.device_path(RECORD).write_text("_data/sub\n")
        sub = self.device_path(DEVICE_DATA + "/sub")
        sub.mkdir(parents=True)
        (sub / "inside.bin").write_bytes(b"not listed")
        done, lines = self.run_tool()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("data_folder_skipped", [l["event"] for l in lines])
        stock = {p[len("_data/"):]: c for p, c in fx.DATA.items()}
        self.assertEqual(self.device_files(), stock)
        self.assertEqual(len(self.aside_dirs()), 1, "the folder with an unlisted file is moved aside, not deleted")
        self.assertEqual((self.aside_dirs()[0] / "sub" / "inside.bin").read_bytes(), b"not listed")

    def test_an_oversized_record_is_refused_before_anything_is_touched(self):
        self.device_path(RECORD).write_text("_data/x\n" * (200 * 1024))  # over the 1 MiB record cap
        done, lines = self.run_tool()
        self.assertEqual(done.returncode, 1)
        self.assertEqual(self.refused(lines), "bad_record")
        self.assertNotIn("push", [c[0] for c in self.calls()])
        self.assertEqual(self.device_files(), {"old_stock.bin": b"data from an earlier install"})

    def test_a_manifest_path_that_leaves_the_data_folder_is_refused_before_any_adb_call(self):
        for bad in ("../x", "/etc/passwd", "_data/a\\b", "_data/a:b", "_data/./x", "_data//x", "_data/x\n", "other/x"):
            with self.subTest(bad=bad):
                self.calls_reset()
                manifest = json.loads(self.layout.manifest.read_text())
                manifest["outputs"]["data"][0]["path"] = bad
                self.layout.manifest.write_text(json.dumps(manifest))
                done, lines = self.run_tool()
                self.assertEqual(done.returncode, 1)
                self.assertEqual(self.refused(lines), "bad_manifest_path")
                self.assertEqual(self.calls(), [])
                self.assertEqual(self.device_files(), {"old_stock.bin": b"data from an earlier install"})

    def calls_reset(self):
        log = self.fake / "calls.log"
        if log.exists():
            log.unlink()
        self.layout.manifest.write_text(json.dumps(json.loads(self.layout.manifest.read_text())))

    def test_no_adb_on_path_is_refused_before_anything_else(self):
        done, lines = self.run_tool(with_adb=False)
        self.assertEqual(done.returncode, 1)
        self.assertEqual(self.refused(lines), "adb_missing")

    def test_no_device_is_refused_and_nothing_but_the_listing_runs(self):
        done, lines = self.run_tool(devices="")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(self.refused(lines), "no_device")
        self.assertEqual([c[0] for c in self.calls()], ["devices"])
        self.assertEqual(self.device_files(), {"old_stock.bin": b"data from an earlier install"})

    def test_a_device_that_is_not_ready_is_refused(self):
        for state in ("unauthorized", "offline"):
            with self.subTest(state):
                done, lines = self.run_tool(devices=f"SERIAL1:{state}")
                self.assertEqual(self.refused(lines), "device_not_ready")
                self.assertIn(state, lines[-1]["detail"])

    def test_two_devices_need_a_serial(self):
        done, lines = self.run_tool(devices="SERIAL1:device,SERIAL2:device")
        self.assertEqual(self.refused(lines), "multiple_devices")
        self.assertNotIn("install", [c[0] for c in self.calls()])
        done, lines = self.run_tool("--serial", "SERIAL2", devices="SERIAL1:device,SERIAL2:device")
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual([l["serial"] for l in lines if l["event"] == "device_chosen"], ["SERIAL2"])
        raw = [json.loads(l) for l in (self.fake / "calls.log").read_text().splitlines()]
        self.assertTrue(all(c[:2] == ["-s", "SERIAL2"] for c in raw[1:]))

    def test_a_serial_that_is_not_attached_is_refused(self):
        done, lines = self.run_tool("--serial", "OTHER")
        self.assertEqual(self.refused(lines), "no_device")
        self.assertIn("OTHER", lines[-1]["detail"])

    def test_an_install_the_headset_refuses_leaves_the_data_alone(self):
        done, lines = self.run_tool(FAKE_ADB_INSTALL="fail")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(self.refused(lines), "install_failed")
        self.assertIn("INSTALL_FAILED_UPDATE_INCOMPATIBLE", lines[-1]["detail"])
        self.assertNotIn("push", [c[0] for c in self.calls()])
        self.assertEqual(self.device_files(), {"old_stock.bin": b"data from an earlier install"})

    def test_an_install_that_says_failure_with_exit_0_is_still_a_refusal(self):
        done, lines = self.run_tool(FAKE_ADB_INSTALL="softfail")
        self.assertEqual(self.refused(lines), "install_failed")
        self.assertIn("INSTALL_FAILED_VERSION_DOWNGRADE", lines[-1]["detail"])
        self.assertNotIn("push", [c[0] for c in self.calls()])

    def test_an_install_that_prints_success_and_exits_non_zero_is_a_refusal(self):
        done, lines = self.run_tool(FAKE_ADB_INSTALL="rc")
        self.assertEqual(self.refused(lines), "install_failed")

    def test_a_push_that_fails_is_refused_and_leaves_no_staging_and_the_old_data(self):
        done, lines = self.run_tool(FAKE_ADB_PUSH="fail")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(self.refused(lines), "push_failed")
        self.assertIn("No space left", lines[-1]["detail"])
        self.assertEqual(self.device_files(), {"old_stock.bin": b"data from an earlier install"})
        self.assertFalse((self.fake / "device" / "sdcard/Android/media/com.readyatdawn.r15/files/.nevr-stock-staging").exists())

    def test_data_that_is_not_on_the_device_after_the_move_is_refused(self):
        done, lines = self.run_tool(FAKE_ADB_LOSE="1")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(self.refused(lines), "device_data_missing")

    def test_a_pushed_file_that_hashes_differently_on_the_device_is_refused_and_removed(self):
        done, lines = self.run_tool(FAKE_ADB_CORRUPT="1")
        self.assertEqual(done.returncode, 1)
        self.assertEqual(self.refused(lines), "device_digest_mismatch")
        self.assertEqual(self.device_files(), {"old_stock.bin": b"data from an earlier install"})  # the data in use is untouched
        self.assertFalse((self.fake / "device" / "sdcard/Android/media/com.readyatdawn.r15/files/.nevr-stock-staging").exists())

    def test_a_store_build_that_was_not_made_is_refused_without_an_adb_call(self):
        self.layout.manifest.unlink()
        done, lines = self.run_tool()
        self.assertEqual(self.refused(lines), "store_build_missing")
        self.assertIn("just quest-store-build", lines[-1]["detail"])
        self.assertEqual(self.calls(), [])

    def test_a_store_build_that_no_longer_matches_its_manifest_is_refused_without_an_adb_call(self):
        victim = self.layout.out / qsb.SIGNED_APK_NAME
        victim.unlink()
        victim.write_bytes(b"not the apk that was built")
        done, lines = self.run_tool()
        self.assertEqual(self.refused(lines), "store_build_stale")
        self.assertEqual(self.calls(), [])

    def test_an_unreadable_manifest_is_refused(self):
        self.layout.manifest.write_text("{")
        done, lines = self.run_tool()
        self.assertEqual(self.refused(lines), "store_build_unreadable")
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
