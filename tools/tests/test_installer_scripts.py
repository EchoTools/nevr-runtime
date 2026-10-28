"""The three installer front ends, each run for real against the real `nevr-install` core and a local server.

Each test builds a throwaway minisign key, a manifest signed with it, a Windows package zip and a local HTTP server, then
runs a script (install.ps1 under pwsh, quest-install.ps1 under pwsh, quest-install.sh under bash) with the core's
`--base`/`--pins`/`--root` pointed at them. The log the core writes is then read: it must hold `run_start`,
`manifest_check` and `run_end`. No headset, no game install and no network: Quest runs use a fake `adb` that records
its calls. The core is built from `installer/` (cargo, locked); without cargo, minisign or pwsh the tests that need
them are skipped by name.
"""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from installer_world import GAME, HOST, REPO, RUNTIME, World, core_binary, sha

PKG = REPO / "tools/package-release"


class ScriptTestBase(unittest.TestCase):
    def setUp(self):
        if shutil.which("minisign") is None:
            self.skipTest("minisign not installed")
        if shutil.which("cargo") is None:
            self.skipTest("cargo not installed: the core cannot be built")
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="installer-scripts-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.core = core_binary()

    def world(self, tamper: bool = False) -> World:
        world = World(self.tmp, tamper)
        self.addCleanup(world.server.close)
        return world

    def package(self, with_sums: bool = True) -> pathlib.Path:
        """A package folder as the Windows zip lays it out: the scripts, the core as nevr-install.exe, SHA256SUMS."""
        pkg = self.tmp / "package"
        pkg.mkdir()
        for name in ("install.ps1", "uninstall.ps1"):
            shutil.copyfile(PKG / name, pkg / name)
        shutil.copyfile(self.core, pkg / "nevr-install.exe")
        os.chmod(pkg / "nevr-install.exe", 0o755)
        if with_sums:
            (pkg / "SHA256SUMS").write_text(f"{sha((pkg / 'nevr-install.exe').read_bytes())}  nevr-install.exe\n")
        return pkg

    def fake_adb(self, with_device: bool = True) -> pathlib.Path:
        """An adb that lists one device and records every other call; nothing is ever pushed anywhere."""
        bindir = self.tmp / "fakebin"
        bindir.mkdir(exist_ok=True)
        adb = bindir / "adb"
        adb.write_text('#!/bin/bash\n'
                       'if [[ "$1" == devices ]]; then printf "List of devices attached\\n' + ('QUEST123\\tdevice\\n' if with_device else '') + '"; exit 0; fi\n'
                       '[[ "$1" == "-s" ]] && shift 2\n'
                       'echo "$*" >> "$FAKE_ADB_LOG"\n'
                       'case "$1" in\n'
                       '  shell) case "$2" in pm) exit 1 ;; df*) printf "Filesystem 1K-blocks Used Available Use%% Mounted\\nx 1 1 9000000 1%% /\\n" ;; esac ;;\n'
                       'esac\n'
                       'exit 0\n')
        adb.chmod(0o755)
        return adb


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class InstallPs1Test(ScriptTestBase):
    def run_install(self, pkg: pathlib.Path, world: World, *extra: str):
        core_args = ",".join(f"'{a}'" for a in world.core_args())
        command = (f"& '{pkg / 'install.ps1'}' -Dir '{world.game}' -NoPause {' '.join(extra)} -CoreArgs {core_args}; exit $LASTEXITCODE")
        return subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=300,
                              env=dict(os.environ, DISPLAY="", WAYLAND_DISPLAY=""))

    def test_a_real_run_installs_the_manifests_bytes_and_logs_the_run(self):
        world, pkg = self.world(), self.package()
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), HOST)
        self.assertEqual((world.game / "nevr-runtime.dll").read_bytes(), RUNTIME)
        events = world.events()
        for want in ("run_start", "manifest_check", "run_end"):
            self.assertIn(want, events)
        end = world.records()[-1]
        self.assertEqual((end["event"], end["state"]), ("run_end", "installed"))
        check = next(r for r in world.records() if r["event"] == "manifest_check")
        self.assertTrue(check["ok"], check)
        self.assertIn("installed", result.stdout)

    def test_the_dlls_in_the_package_that_match_the_manifest_are_installed_without_a_download(self):
        world = World(self.tmp, serve_zip=False, name="nozip")
        self.addCleanup(world.server.close)
        pkg = self.package()
        (pkg / "BugSplat64.dll").write_bytes(HOST)
        (pkg / "nevr-runtime.dll").write_bytes(RUNTIME)
        core_args = ",".join(f"'{a}'" for a in world.core_args())
        command = f"& '{pkg / 'install.ps1'}' -Dir '{world.game}' -NoPause -CoreArgs {core_args}; exit $LASTEXITCODE"
        result = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), HOST)
        local = next(r for r in world.records() if r["event"] == "package_local")
        self.assertTrue(local["ok"], local)
        self.assertNotIn("artifact_fetch", world.events())

    def test_a_package_dll_that_differs_from_the_manifest_is_replaced_by_the_manifests_download(self):
        world, pkg = self.world(), self.package()
        (pkg / "BugSplat64.dll").write_bytes(b"tampered")
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), HOST)
        self.assertIn("artifact_fetch", world.events())

    def test_uninstall_ps1_undoes_what_the_core_installed(self):
        world, pkg = self.world(), self.package()
        self.assertEqual(self.run_install(pkg, world).returncode, 0)
        undone = subprocess.run(["pwsh", "-NoProfile", "-File", str(pkg / "uninstall.ps1"), "-Dir", str(world.game)],
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(undone.returncode, 0, undone.stdout + undone.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), GAME)

    def test_a_damaged_core_is_named_before_it_runs(self):
        world, pkg = self.world(), self.package()
        (pkg / "SHA256SUMS").write_text(f"{'0' * 64}  nevr-install.exe\n")
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("nevr-install.exe does not match SHA256SUMS", result.stdout + result.stderr)
        self.assertEqual(world.records(), [], "the core must not have run")
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), GAME)

    def test_a_manifest_that_does_not_verify_changes_nothing(self):
        world, pkg = self.world(tamper=True), self.package()
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), GAME)
        self.assertFalse((world.game / "nevr-runtime.dll").exists())
        end = world.records()[-1]
        self.assertEqual((end["event"], end["state"], end["failed"]), ("run_end", "unchanged", True))

    def test_the_script_carries_no_install_logic_of_its_own(self):
        text = (PKG / "install.ps1").read_text()
        for needle in ("Copy-Item", "Rename-Item", "WriteAllBytes", "Write-State"):
            self.assertNotIn(needle, text)


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class QuestInstallPs1Test(ScriptTestBase):
    def test_a_real_run_reaches_the_core_and_changes_nothing_when_the_release_has_no_apk(self):
        world, adb = self.world(), self.fake_adb()
        script = self.tmp / "quest-install.ps1"
        shutil.copyfile(REPO / "tools/quest-install.ps1", script)
        adb_log = self.tmp / "adb.log"
        adb_log.write_text("")
        core_args = ",".join(f"'{a}'" for a in world.core_args())
        command = f"& '{script}' -Core '{self.core}' -Adb '{adb}' -NoPause -CoreArgs {core_args}; exit $LASTEXITCODE"
        result = subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=300,
                                env=dict(os.environ, FAKE_ADB_LOG=str(adb_log)))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        events = world.events()
        for want in ("run_start", "manifest_check", "run_end"):
            self.assertIn(want, events)
        self.assertEqual(world.records()[-1]["event"], "run_end")
        self.assertFalse(any(c.startswith(("install", "uninstall", "push")) for c in adb_log.read_text().splitlines()),
                         adb_log.read_text())


class QuestInstallShTest(ScriptTestBase):
    SCRIPT = REPO / "tools/quest-install.sh"

    def run_script(self, world: World, *args: str):
        adb = self.fake_adb()
        log = self.tmp / "adb.log"
        log.write_text("")
        env = dict(os.environ, NEVR_INSTALL=str(self.core), PATH=f"{adb.parent}:{os.environ['PATH']}", FAKE_ADB_LOG=str(log))
        result = subprocess.run([str(self.SCRIPT), *args, *world.core_args()], capture_output=True, text=True, timeout=300, env=env)
        return result, log.read_text().splitlines()

    def test_a_real_run_reaches_the_core_and_changes_nothing_when_the_release_has_no_apk(self):
        world = self.world()
        result, adb_calls = self.run_script(world)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for want in ("run_start", "manifest_check", "run_end"):
            self.assertIn(want, world.events())
        self.assertEqual(world.records()[-1]["event"], "run_end")
        self.assertFalse(any(c.startswith(("install", "uninstall", "push")) for c in adb_calls), adb_calls)
        self.assertEqual(json.loads(result.stdout.strip().splitlines()[-1])["log"], str(world.root / "logs/nevr-install.jsonl"))

    def test_a_manifest_that_does_not_verify_stops_before_any_device_call(self):
        world = self.world(tamper=True)
        result, adb_calls = self.run_script(world)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(world.records()[-1]["state"], "unchanged")
        self.assertEqual(adb_calls, [])

    def test_the_old_yes_argument_is_refused_not_ignored(self):
        world = self.world()
        result, adb_calls = self.run_script(world, "yes")
        self.assertEqual(result.returncode, 1)
        self.assertIn("--yes-uninstall", result.stderr)
        self.assertEqual(world.records(), [])
        self.assertEqual(adb_calls, [])

    def test_a_directory_argument_becomes_the_artifact_dir(self):
        stub = self.tmp / "stub-core"
        out = self.tmp / "args.txt"
        stub.write_text('#!/bin/bash\nprintf "%s\\n" "$@" > "$STUB_OUT"\n')
        stub.chmod(0o755)
        apk_dir = self.tmp / "apks"
        apk_dir.mkdir()
        result = subprocess.run([str(self.SCRIPT), str(apk_dir), "--yes-uninstall"], capture_output=True, text=True, timeout=30,
                                env=dict(os.environ, NEVR_INSTALL=str(stub), STUB_OUT=str(out)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(out.read_text().splitlines(), ["quest", "install", "--artifact-dir", str(apk_dir), "--yes-uninstall"])

    def test_without_a_core_the_script_says_so(self):
        env = {k: v for k, v in os.environ.items() if k != "NEVR_INSTALL"}
        env["PATH"] = "/usr/bin:/bin"
        env = {**env, "PATH": f"{self.tmp}:/usr/bin:/bin"}
        result = subprocess.run(["/bin/bash", str(self.SCRIPT)], capture_output=True, text=True, timeout=30, env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn("nevr-install not found", result.stderr)


if __name__ == "__main__":
    unittest.main()
