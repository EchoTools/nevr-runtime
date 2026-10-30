"""The three installer front ends, each run for real against the real `install-nevr-runtime` core and a local server.

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
import sys
import unittest

from installer_world import GAME, HOST, REPO, RUNTIME, World, core_binary, sha

sys.path.insert(0, str(REPO / "tools"))
import game_folder_invariant as invariant  # noqa: E402

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
        """A package folder as the Windows zip lays it out: the scripts, the core as install-nevr-runtime.exe, SHA256SUMS."""
        pkg = self.tmp / "package"
        pkg.mkdir()
        for name in ("install.ps1", "uninstall.ps1"):
            shutil.copyfile(PKG / name, pkg / name)
        shutil.copyfile(self.core, pkg / "install-nevr-runtime.exe")
        os.chmod(pkg / "install-nevr-runtime.exe", 0o755)
        if with_sums:
            (pkg / "SHA256SUMS").write_text(f"{sha((pkg / 'install-nevr-runtime.exe').read_bytes())}  install-nevr-runtime.exe\n")
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
        command = (f"& '{pkg / 'install.ps1'}' -Dir '{world.game}' -Root '{world.nevr}' -NoPause {' '.join(extra)} -CoreArgs {core_args}; exit $LASTEXITCODE")
        return subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=300,
                              env=dict(os.environ, DISPLAY="", WAYLAND_DISPLAY=""))

    def run_uninstall(self, pkg: pathlib.Path, world: World):
        return subprocess.run(["pwsh", "-NoProfile", "-File", str(pkg / "uninstall.ps1"), "-Dir", str(world.game),
                               "-Root", str(world.nevr), "-NoPause"], capture_output=True, text=True, timeout=120,
                              env=dict(os.environ, DISPLAY="", WAYLAND_DISPLAY=""))

    def test_a_real_run_installs_the_manifests_bytes_and_logs_the_run(self):
        world, pkg = self.world(), self.package()
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), HOST)
        self.assertEqual((world.nevr / "nevr-runtime.dll").read_bytes(), RUNTIME)
        self.assertTrue((world.nevr / "installer/nevr-install.txt").is_file())
        self.assertEqual(world.game_files(), ["BugSplat64.dll", "echovr.exe"], "nothing of ours but the host is in the game folder")
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
        command = f"& '{pkg / 'install.ps1'}' -Dir '{world.game}' -Root '{world.nevr}' -NoPause -CoreArgs {core_args}; exit $LASTEXITCODE"
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
        undone = self.run_uninstall(pkg, world)
        self.assertEqual(undone.returncode, 0, undone.stdout + undone.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), GAME)
        self.assertFalse((world.nevr / "nevr-runtime.dll").exists(), "the runtime is set aside in the nevr folder")
        self.assertEqual(len(list((world.nevr / "installer/aside").glob("nevr-runtime.dll.uninstalled-*"))), 1)
        self.assertEqual(world.game_files(), ["BugSplat64.dll", "echovr.exe"], "uninstall leaves nothing of ours in the game folder")

    def assert_a_record_less_nevr_host_is_adopted(self, old_host: bytes):
        world, pkg = self.world(), self.package()
        (world.game / "BugSplat64.dll").write_bytes(old_host)
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        plan = next(r for r in world.records() if r["event"] == "plan")
        self.assertEqual((plan["action"], plan["decision"]), ("Adopt", "adopt"), plan)
        end = world.records()[-1]
        self.assertEqual((end["event"], end["state"]), ("run_end", "installed"))
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), HOST)
        self.assertEqual((world.nevr / "nevr-runtime.dll").read_bytes(), RUNTIME)
        record = dict(line.split("=", 1) for line in (world.nevr / "installer/nevr-install.txt").read_text().splitlines() if "=" in line)
        self.assertEqual(record.get("original"), "unknown", "the game's own host was never kept")
        self.assertEqual(record.get("previous_sha256"), sha(old_host))
        self.assertEqual(world.game_files(), ["BugSplat64.dll", "echovr.exe"])

    # An earlier NEVR put in place by another installer leaves a runtime host and no install record. Shaped like the shipped
    # hosts: the stamp `NEVR-BUILD <version> <commit40>` inside an MZ blob (all 26 shipped hosts, v5.1.0 to v5.18.0, carry it
    # once), with the CDN agent string too on the monolithic ones (v5.1.0 to v5.6.0) and without it on the thin ones (v5.7.2 on).
    # install.ps1 used to end on "nevr-install.txt is missing; the file was left as it is" for these.
    STAMP = b"NEVR-BUILD 5.4.3 " + b"b" * 40 + b"\0"

    def test_a_thin_nevr_host_with_no_record_is_adopted_not_refused(self):
        self.assert_a_record_less_nevr_host_is_adopted(b"MZ\0" + self.STAMP + b"\xcc" * 64)

    def test_a_monolithic_nevr_host_with_no_record_is_adopted_not_refused(self):
        self.assert_a_record_less_nevr_host_is_adopted(b"MZ\0" + self.STAMP + b"\xcc" * 64 + b"nevr-runtime/1.0\0")

    def test_a_nevr_host_older_than_the_stamp_with_no_record_is_adopted_not_refused(self):
        # #785: a runtime built before the NEVR-BUILD literal carries only the CDN agent string.
        self.assert_a_record_less_nevr_host_is_adopted(b"MZ\0" + b"\xcc" * 64 + b"nevr-runtime/1.0\0")

    def test_a_damaged_core_is_named_before_it_runs(self):
        world, pkg = self.world(), self.package()
        (pkg / "SHA256SUMS").write_text(f"{'0' * 64}  install-nevr-runtime.exe\n")
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("install-nevr-runtime.exe does not match SHA256SUMS", result.stdout + result.stderr)
        self.assertEqual(world.records(), [], "the core must not have run")
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), GAME)

    def test_a_manifest_that_does_not_verify_changes_nothing(self):
        world, pkg = self.world(tamper=True), self.package()
        result = self.run_install(pkg, world)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual((world.game / "BugSplat64.dll").read_bytes(), GAME)
        self.assertFalse((world.nevr / "nevr-runtime.dll").exists())
        self.assertEqual(world.game_files(), ["BugSplat64.dll", "echovr.exe"])
        end = world.records()[-1]
        self.assertEqual((end["event"], end["state"], end["failed"]), ("run_end", "unchanged", True))

    def test_the_script_carries_no_install_logic_of_its_own(self):
        text = (PKG / "install.ps1").read_text()
        for needle in ("Copy-Item", "Rename-Item", "WriteAllBytes", "Write-State"):
            self.assertNotIn(needle, text)


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class GameFolderInvariantTest(ScriptTestBase):
    """The standing rule: nothing of ours is written to the game install except BugSplat64.dll (AGENTS.md, "The game folder").

    The install root is snapshotted before and after install, an update, a client run, a `-server` run and the uninstall; only the
    content of `bin/win10/BugSplat64.dll` may differ from the first snapshot, and after the uninstall nothing does. The two game
    runs are filled in with NEVR_INVARIANT_CLIENT_RUN and NEVR_INVARIANT_SERVER_RUN (tools/game_folder_invariant.py). A run whose
    variable is unset is printed as NOT RUN: this test says nothing about it."""

    def fake_install(self) -> pathlib.Path:
        install = self.tmp / "ready-at-dawn-echo-arena"
        (install / "bin/win10").mkdir(parents=True)
        (install / "bin/win10/echovr.exe").write_bytes(b"exe")
        (install / "bin/win10/BugSplat64.dll").write_bytes(GAME)
        (install / "_local/r14logs").mkdir(parents=True)
        (install / "_local/r14logs/old.log").write_text("the game's own log")
        (install / "_local/config.json").write_text("{}")
        return install

    def script(self, pkg: pathlib.Path, name: str, game: pathlib.Path, world: World, *extra: str) -> subprocess.CompletedProcess:
        core_args = ",".join(f"'{a}'" for a in world.core_args())
        tail = f" -CoreArgs {core_args}" if name == "install.ps1" else ""
        command = f"& '{pkg / name}' -Dir '{game}' -Root '{world.nevr}' -NoPause {' '.join(extra)}{tail}; exit $LASTEXITCODE"
        return subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=300,
                              env=dict(os.environ, DISPLAY="", WAYLAND_DISPLAY=""))

    def test_install_update_client_run_server_run_and_uninstall_change_nothing_but_the_hosts_content(self):
        world, pkg = self.world(), self.package()
        second = World(self.tmp, host=b"second host NEVR-BUILD 9.9.10", runtime=b"second runtime NEVR-BUILD 9.9.10",
                       manifest_version=6, name="w2")
        self.addCleanup(second.server.close)
        install = self.fake_install()
        game = install / "bin/win10"
        initial = invariant.snapshot(install)

        def run_script(name, using):
            def step(_root):
                done = self.script(pkg, name, game, using)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            return step

        results = invariant.run_steps(install, [
            ("install", run_script("install.ps1", world)),
            ("client run", invariant.command_step(invariant.CLIENT_RUN_ENV)),
            ("update", run_script("install.ps1", second)),
            ("server run", invariant.command_step(invariant.SERVER_RUN_ENV)),
            ("uninstall", run_script("uninstall.ps1", world)),
        ])
        self.assertEqual([r.name for r in results], ["install", "client run", "update", "server run", "uninstall"])
        for r in results:
            if r.ran:
                self.assertTrue(r.ok, f"step '{r.name}': {r.error or r.violations}")
        for must in ("install", "update", "uninstall"):
            self.assertTrue(next(r for r in results if r.name == must).ran, must)
        self.assertEqual(invariant.snapshot(install), initial, "after the uninstall the install is exactly as it was")
        self.assertEqual((game / "BugSplat64.dll").read_bytes(), GAME)
        self.assertTrue((world.nevr / "installer/aside").is_dir(), "the set-aside files went to the nevr folder")


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

    def test_an_apk_argument_is_staged_with_its_sidecar_for_the_core_and_the_staging_is_removed(self):
        stub = self.tmp / "stub-core"
        out = self.tmp / "args.txt"
        stub.write_text('#!/bin/bash\nprintf "%s\\n" "$@" > "$STUB_OUT"\nd=$4\nls "$d" >> "$STUB_OUT"\ncat "$d"/*.sha256 >> "$STUB_OUT"\nprintf "%s\\n" "$d" > "$STUB_OUT.dir"\n')
        stub.chmod(0o755)
        apk = self.tmp / "mine.apk"
        apk.write_bytes(b"locally built apk")
        result = subprocess.run([str(self.SCRIPT), str(apk), "--serial", "Q1"], capture_output=True, text=True, timeout=30,
                                env=dict(os.environ, NEVR_INSTALL=str(stub), STUB_OUT=str(out)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = out.read_text().splitlines()
        staged = pathlib.Path((self.tmp / "args.txt.dir").read_text().strip())
        self.assertEqual(lines[:2], ["quest", "install"])
        self.assertEqual(lines[2], "--artifact-dir")
        self.assertEqual(lines[3], str(staged))
        self.assertEqual(lines[4:6], ["--serial", "Q1"])
        self.assertEqual(lines[6:8], ["mine.apk", "mine.apk.sha256"])
        self.assertEqual(lines[8], f"{sha(b"locally built apk")}  mine.apk")
        self.assertFalse(staged.exists(), "the staging directory is removed when the run ends")
        self.assertEqual(apk.read_bytes(), b"locally built apk", "the caller's file is untouched")

    def test_without_NEVR_INSTALL_the_script_finds_the_core_on_PATH_under_its_new_name(self):
        """#1492: the core on PATH is install-nevr-runtime; a program named nevr-install is not looked for."""
        out = self.tmp / "args.txt"
        for name, found in (("install-nevr-runtime", True), ("nevr-install", False)):
            bin_dir = self.tmp / f"bin-{name}"
            bin_dir.mkdir()
            stub = bin_dir / name
            stub.write_text('#!/bin/bash\nprintf "%s\\n" "$@" > "$STUB_OUT"\n')
            stub.chmod(0o755)
            apk_dir = self.tmp / "apks"
            apk_dir.mkdir(exist_ok=True)
            env = {k: v for k, v in os.environ.items() if k != "NEVR_INSTALL"}
            env.update(PATH=f"{bin_dir}:/usr/bin:/bin", STUB_OUT=str(out))
            out.unlink(missing_ok=True)
            result = subprocess.run(["/bin/bash", str(self.SCRIPT), str(apk_dir)], capture_output=True, text=True, timeout=30, env=env)
            if found:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(out.read_text().splitlines(), ["quest", "install", "--artifact-dir", str(apk_dir)])
            else:
                self.assertEqual(result.returncode, 1)
                self.assertFalse(out.exists(), "the old name on PATH is not run")

    def test_without_a_core_the_script_says_so(self):
        env = {k: v for k, v in os.environ.items() if k != "NEVR_INSTALL"}
        env["PATH"] = "/usr/bin:/bin"
        env = {**env, "PATH": f"{self.tmp}:/usr/bin:/bin"}
        result = subprocess.run(["/bin/bash", str(self.SCRIPT)], capture_output=True, text=True, timeout=30, env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn("install-nevr-runtime not found", result.stderr)
        self.assertIn("build it with 'just build-installer' and put it on PATH", result.stderr, "the hint is text, not a command the script runs")


if __name__ == "__main__":
    unittest.main()
