"""package_rc.py: the release-candidate gate and assembly, with fake artifacts; install.ps1/uninstall.ps1 under pwsh."""

import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "package_rc.py"
PACKAGE = REPO / "tools" / "package-rc"

COMMIT = "3a35e0b945456dba8a9b56e8110df330178e7259"
VALUES = {
    "NEVR_SOCKET_URI": "wss://host.example/nevr",
    "NEVR_HTTP_URI": "https://host.example",
    "NEVR_PUBLIC_API_KEY": "public-api-key-value",
    "NEVR_PUBLIC_SOCKET_KEY": "public-socket-key-value",
}
NAMES = {"NEVR_SOCKET_URI": "kSocketUri", "NEVR_HTTP_URI": "kHttpUri",
         "NEVR_PUBLIC_API_KEY": "kPublicApiKey", "NEVR_PUBLIC_SOCKET_KEY": "kPublicSocketKey"}


def defaults_header(values):
    return "namespace nevr_builtin {\n" + "".join(
        f'inline constexpr const char* {NAMES[k]} = "{v}";\n' for k, v in values.items()) + "}\n"


def build_info(version, features):
    return (f'inline constexpr const char* kVersion = "{version}";\n'
            f'inline constexpr const char* kCommit = "{COMMIT[:8]}";\n'
            f'inline constexpr const char* kDefaultFeatures = "{features}";\n')


class PackageRcTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="package-rc-test-"))
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.version = "4.0.0-rc.3+1172.3a35e0b9"
        self.defaults = self.tmp / "public-defaults.env"
        self.defaults.write_text("".join(f"{k}={v}\n" for k, v in VALUES.items()))
        self.pc_header = self.tmp / "pc.h"
        self.pc_header.write_text(defaults_header(VALUES))
        self.quest_header = self.tmp / "quest.h"
        self.quest_header.write_text(defaults_header(VALUES))
        self.info = self.tmp / "info.h"
        self.info.write_text(build_info(self.version, "redirect,bridge,login,social"))
        blob = b"\0".join([v.encode() for v in VALUES.values()] + [self.version.encode(), COMMIT[:8].encode()])
        self.dll = self.tmp / "BugSplat64.dll"
        self.dll.write_bytes(b"MZ" + blob)
        self.apk = self.tmp / "signed.apk"
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", b"\x7fELF" + blob)
            z.writestr("AndroidManifest.xml", b"<m/>")
        self.out = self.tmp / "out"

    def run_tool(self, n=3, **overrides):
        args = {"--n": str(n), "--commit": COMMIT, "--out": str(self.out), "--dll": str(self.dll),
                "--apk": str(self.apk), "--pc-header": str(self.pc_header),
                "--quest-header": str(self.quest_header), "--quest-build-info": str(self.info),
                "--defaults": str(self.defaults)}
        args.update(overrides)
        flat = [x for kv in args.items() for x in kv]
        return subprocess.run([sys.executable, "-I", str(TOOL), *flat], capture_output=True, text=True)

    def test_a_clean_candidate_is_assembled_with_checksums_and_notes(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        zip_path = self.out / "nevr-runtime-v4.0.0-rc.3-windows.zip"
        apk_path = self.out / "nevr-runtime-v4.0.0-rc.3-quest.apk"
        self.assertTrue(zip_path.exists() and apk_path.exists())
        with zipfile.ZipFile(zip_path) as z:
            self.assertEqual(sorted(z.namelist()),
                             ["BugSplat64.dll", "README.txt", "SHA256SUMS", "install.ps1", "uninstall.ps1"])
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            for name in z.namelist():
                if name != "SHA256SUMS":
                    self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)
            self.assertIn(self.version, z.read("README.txt").decode())
        top = (self.out / "SHA256SUMS").read_text()
        self.assertIn(hashlib.sha256(zip_path.read_bytes()).hexdigest(), top)
        self.assertIn(hashlib.sha256(apk_path.read_bytes()).hexdigest(), top)
        notes = (self.out / "RELEASE-NOTES.md").read_text()
        for needle in ("unsigned", "install.ps1", "uninstall.ps1", self.version, zip_path.name, apk_path.name):
            self.assertIn(needle, notes.replace("**Unsigned**", "unsigned").replace("**unsigned**", "unsigned"))

    def test_a_dll_that_embeds_no_endpoints_is_refused_and_nothing_is_written(self):
        self.dll.write_bytes(b"MZ" + self.version.encode() + COMMIT[:8].encode())
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: NEVR_SOCKET_URI: not embedded in BugSplat64.dll", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_quest_build_that_needs_a_config_file_to_log_in_is_refused(self):
        self.info.write_text(build_info(self.version, ""))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("quest: feature login is not on by default", result.stderr)
        self.assertFalse(self.out.exists())

    def test_the_apk_sentinel_must_embed_the_values_too(self):
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", b"\x7fELF" + self.version.encode() + COMMIT[:8].encode())
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("quest: NEVR_HTTP_URI: not embedded in libovrplatformloader.so", result.stderr)

    def test_a_binary_without_the_rc_label_or_the_commit_is_refused(self):
        blob = b"\0".join(v.encode() for v in VALUES.values())
        self.dll.write_bytes(b"MZ" + blob + b"\0" + b"4.0.0+1172.3a35e0b9")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: no '-rc.3+' version string in the binary", result.stderr)

    def test_the_label_must_match_n(self):
        result = self.run_tool(n=4)
        self.assertEqual(result.returncode, 1)
        self.assertIn("-rc.4", result.stderr)

    def test_an_existing_artifact_is_never_overwritten(self):
        self.assertEqual(self.run_tool().returncode, 0)
        again = self.run_tool()
        self.assertEqual(again.returncode, 1)
        self.assertIn("exists; not overwritten", again.stderr)


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class InstallScriptsTest(unittest.TestCase):
    """install.ps1 and uninstall.ps1 run for real under pwsh against a fake install directory."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="package-rc-ps-"))
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.pkg = self.tmp / "pkg"
        self.pkg.mkdir()
        self.game = self.tmp / "game" / "bin" / "win10"
        self.game.mkdir(parents=True)
        (self.game / "echovr.exe").write_bytes(b"exe")
        (self.game / "BugSplat64.dll").write_bytes(b"ORIGINAL")
        (self.game / "dbgcore.dll").write_bytes(b"LEGACY")
        for name in ("install.ps1", "uninstall.ps1"):
            shutil.copy(PACKAGE / name, self.pkg / name)
        (self.pkg / "BugSplat64.dll").write_bytes(b"NEW-RUNTIME")
        digest = hashlib.sha256(b"NEW-RUNTIME").hexdigest()
        (self.pkg / "SHA256SUMS").write_text(f"{digest}  BugSplat64.dll\n")

    def ps(self, script, *args):
        return subprocess.run(["pwsh", "-NoProfile", "-File", str(self.pkg / script), *args],
                              capture_output=True, text=True, timeout=60)

    def test_install_sets_the_originals_aside_and_uninstall_puts_them_back_without_deleting(self):
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"NEW-RUNTIME")
        self.assertFalse((self.game / "dbgcore.dll").exists())
        backups = list(self.game.glob("BugSplat64.dll.original-*"))
        legacy = list(self.game.glob("dbgcore.dll.legacy-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(len(legacy), 1)
        self.assertEqual(backups[0].read_bytes(), b"ORIGINAL")
        self.assertEqual(legacy[0].read_bytes(), b"LEGACY")
        again = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(again.returncode, 0, "a second install must refuse")
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, undone.stdout + undone.stderr)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertEqual((self.game / "dbgcore.dll").read_bytes(), b"LEGACY")
        self.assertTrue(backups[0].exists(), "the backup is kept")

    def test_a_package_that_does_not_match_its_sums_is_refused_and_nothing_changes(self):
        (self.pkg / "BugSplat64.dll").write_bytes(b"TAMPERED")
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertTrue((self.game / "dbgcore.dll").exists())

    def test_no_script_deletes_anything(self):
        for name in ("install.ps1", "uninstall.ps1"):
            text = (PACKAGE / name).read_text()
            self.assertNotIn("Remove-Item", text, name)
            self.assertNotRegex(text, r"\bdel\b|\brm\b", name)


if __name__ == "__main__":
    unittest.main()
