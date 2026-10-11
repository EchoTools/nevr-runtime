"""package_rc.py: the package gate and assembly, with fake artifacts; install.ps1/uninstall.ps1 under pwsh.

A local build stamps -dev and is packaged as nevr-runtime-v4.0.0-dev-<sha>-*; only the CI stages (tree, seal,
stamp) deal in -rc.<N>."""

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


class PackageFixture(unittest.TestCase):
    """Fake artifacts for the tool. VERSION is the stamp the fake binaries carry."""
    VERSION = "4.0.0-dev+1172.3a35e0b9"

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="package-rc-test-"))
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.version = self.VERSION
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

    def run_tool(self, **overrides):
        """The local path (`just package-dev`): no --n, the binaries must carry -dev."""
        args = {"--commit": COMMIT, "--out": str(self.out), "--dll": str(self.dll),
                "--apk": str(self.apk), "--pc-header": str(self.pc_header),
                "--quest-header": str(self.quest_header), "--quest-build-info": str(self.info),
                "--defaults": str(self.defaults)}
        args.update(overrides)
        flat = [x for kv in args.items() for x in kv]
        return subprocess.run([sys.executable, "-I", str(TOOL), *flat], capture_output=True, text=True)


class PackageRcTest(PackageFixture):
    """The local build: a dev stamp, dev-named files, never an -rc.<N> artifact."""

    def test_a_clean_local_build_is_packaged_as_a_development_build_with_checksums_and_notes(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        zip_path = self.out / "nevr-runtime-v4.0.0-dev-3a35e0b-windows.zip"
        apk_path = self.out / "nevr-runtime-v4.0.0-dev-3a35e0b-quest.apk"
        self.assertTrue(zip_path.exists() and apk_path.exists())
        with zipfile.ZipFile(zip_path) as z:
            self.assertEqual(sorted(z.namelist()),
                             ["BugSplat64.dll", "README.txt", "SHA256SUMS", "SIGNING.txt", "install.ps1",
                              "uninstall.ps1"])
            self.assertTrue(z.read("SIGNING.txt").decode().startswith("UNSIGNED\n"))
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            for name in z.namelist():
                if name != "SHA256SUMS":
                    self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)
            self.assertIn(self.version, z.read("README.txt").decode())
        top = (self.out / "SHA256SUMS").read_text()
        self.assertIn(hashlib.sha256(zip_path.read_bytes()).hexdigest(), top)
        self.assertIn(hashlib.sha256(apk_path.read_bytes()).hexdigest(), top)
        notes = (self.out / "RELEASE-NOTES.md").read_text()
        for needle in ("**UNSIGNED.**", "install.ps1", "uninstall.ps1", self.version, zip_path.name, apk_path.name,
                       "development build, NOT a release candidate"):
            self.assertIn(needle, notes)
        self.assertNotIn("SIGNED.**", notes.replace("UNSIGNED.**", ""))  # nothing is labelled signed
        self.assertEqual([p.name for p in self.out.iterdir() if "-rc." in p.name], [])  # no rc-named artifact
        with zipfile.ZipFile(zip_path) as z:
            self.assertIn("development build, NOT a release candidate", z.read("README.txt").decode())

    def test_a_local_run_refuses_a_binary_stamped_rc(self):
        rc = "4.0.0-rc.3+1172.3a35e0b9"
        blob = b"\0".join([v.encode() for v in VALUES.values()] + [rc.encode(), COMMIT[:8].encode()])
        self.dll.write_bytes(b"MZ" + blob)
        self.info.write_text(build_info(rc, "redirect,bridge,login,social"))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("a local build is never a release candidate", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_local_run_has_no_n_option(self):
        result = self.run_tool(**{"--n": "3"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments: --n", result.stderr)
        self.assertFalse(self.out.exists())

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

    def test_a_binary_without_the_dev_stamp_or_the_commit_is_refused(self):
        blob = b"\0".join(v.encode() for v in VALUES.values())
        self.dll.write_bytes(b"MZ" + blob + b"\0" + b"4.0.0+1172.3a35e0b9")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: no '-dev+' version string in the binary", result.stderr)

    def test_an_existing_artifact_is_never_overwritten(self):
        self.assertEqual(self.run_tool().returncode, 0)
        again = self.run_tool()
        self.assertEqual(again.returncode, 1)
        self.assertIn("exists; not overwritten", again.stderr)


def fake_pe(certificate_size: int, payload: bytes) -> bytes:
    """A minimal PE32+ header (no sections) with the Authenticode directory entry set, then `payload`."""
    image = bytearray(0x200)
    image[0:2] = b"MZ"
    image[0x3C:0x40] = (0x80).to_bytes(4, "little")
    image[0x80:0x84] = b"PE\0\0"
    optional = 0x80 + 24
    image[optional:optional + 2] = (0x20B).to_bytes(2, "little")
    entry = optional + 112 + 8 * 4
    image[entry:entry + 4] = (0x1F0).to_bytes(4, "little")
    image[entry + 4:entry + 8] = certificate_size.to_bytes(4, "little")
    return bytes(image) + payload


class SigningStagesTest(PackageFixture):
    """The CI release path: gate and write the tree, sign it (here: a stand-in), then seal."""
    VERSION = "4.0.0-rc.3+1172.3a35e0b9"

    def run_stage(self, stage, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL), stage, *args], capture_output=True, text=True)

    def tree_args(self, **overrides):
        args = {"--n": "3", "--commit": COMMIT, "--out": str(self.out), "--dll": str(self.dll),
                "--pc-header": str(self.pc_header), "--defaults": str(self.defaults)}
        args.update(overrides)
        return [x for kv in args.items() for x in kv]

    def make_tree(self):
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.out / "nevr-runtime-v4.0.0-rc.3-windows"

    def test_tree_holds_the_files_the_sign_job_signs_and_no_checksums_yet(self):
        tree = self.make_tree()
        self.assertEqual(sorted(p.name for p in tree.iterdir()),
                         ["BugSplat64.dll", "README.txt", "install.ps1", "uninstall.ps1"])
        self.assertTrue((self.out / "rc.json").exists())

    def test_the_tree_stage_refuses_a_dll_that_embeds_no_defaults(self):
        self.dll.write_bytes(b"MZ" + self.version.encode() + COMMIT[:8].encode())
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: NEVR_SOCKET_URI: not embedded in BugSplat64.dll", result.stderr)

    def sign_scripts(self, tree):
        """What signing does to a PowerShell script: an Authenticode block is appended."""
        for name in ("install.ps1", "uninstall.ps1"):
            path = tree / name
            path.write_bytes(path.read_bytes() + b"\n# SIG # Begin signature block\n# SIG # End signature block\n")

    def test_seal_regenerates_sha256sums_from_the_signed_files(self):
        tree = self.make_tree()
        unsigned_sha = hashlib.sha256((tree / "BugSplat64.dll").read_bytes()).hexdigest()
        signed = fake_pe(0x100, (tree / "BugSplat64.dll").read_bytes())  # the sign job rewrites the file
        (tree / "BugSplat64.dll").write_bytes(signed)
        self.sign_scripts(tree)
        sealed = self.tmp / "sealed"
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(sealed), "--require-signed")
        self.assertEqual(result.returncode, 0, result.stderr)
        zip_path = sealed / "nevr-runtime-v4.0.0-rc.3-windows.zip"
        with zipfile.ZipFile(zip_path) as z:
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            self.assertEqual(sums["BugSplat64.dll"], hashlib.sha256(signed).hexdigest())
            self.assertNotEqual(sums["BugSplat64.dll"], unsigned_sha)
            self.assertEqual(z.read("BugSplat64.dll"), signed)
            signing = z.read("SIGNING.txt").decode()
            self.assertTrue(signing.startswith("SIGNED\n"))
            for name in ("BugSplat64.dll", "install.ps1", "uninstall.ps1"):
                self.assertIn(name, signing)
            for name in z.namelist():
                if name != "SHA256SUMS":
                    self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)
        top = (sealed / "SHA256SUMS").read_text()
        self.assertIn(f"{hashlib.sha256(zip_path.read_bytes()).hexdigest()}  {zip_path.name}", top)
        self.assertIn("**SIGNED.**", (sealed / "RELEASE-NOTES.md").read_text())

    def test_seal_without_require_signed_never_calls_the_files_signed(self):
        tree = self.make_tree()
        (tree / "BugSplat64.dll").write_bytes(fake_pe(0x100, b"x"))  # even a file that happens to carry a signature
        self.sign_scripts(tree)
        sealed = self.tmp / "sealed"
        self.assertEqual(self.run_stage("seal", "--tree", str(tree), "--out", str(sealed)).returncode, 0)
        with zipfile.ZipFile(sealed / "nevr-runtime-v4.0.0-rc.3-windows.zip") as z:
            self.assertTrue(z.read("SIGNING.txt").decode().startswith("UNSIGNED\n"))
        self.assertIn("**UNSIGNED.**", (sealed / "RELEASE-NOTES.md").read_text())

    def test_seal_requiring_signatures_refuses_an_unsigned_tree(self):
        tree = self.make_tree()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "sealed"), "--require-signed")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not signed (no certificate table): BugSplat64.dll", result.stderr)
        self.assertFalse((self.tmp / "sealed").exists())

    def test_seal_requiring_signatures_refuses_a_signed_dll_with_unsigned_install_scripts(self):
        tree = self.make_tree()
        (tree / "BugSplat64.dll").write_bytes(fake_pe(0x100, b"x"))
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "sealed"), "--require-signed")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not signed (no signature block): install.ps1, uninstall.ps1", result.stderr)

    def test_seal_puts_a_given_apk_under_the_checksums(self):
        tree = self.make_tree()
        sealed = self.tmp / "sealed"
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(sealed), "--apk", str(self.apk))
        self.assertEqual(result.returncode, 0, result.stderr)
        apk = sealed / "nevr-runtime-v4.0.0-rc.3-quest.apk"
        self.assertEqual(apk.read_bytes(), self.apk.read_bytes())
        self.assertIn(f"{hashlib.sha256(apk.read_bytes()).hexdigest()}  {apk.name}", (sealed / "SHA256SUMS").read_text())

    def test_seal_without_an_apk_makes_a_zip_only_candidate_and_says_so(self):
        tree = self.make_tree()
        sealed = self.tmp / "sealed"
        self.assertEqual(self.run_stage("seal", "--tree", str(tree), "--out", str(sealed)).returncode, 0)
        self.assertNotIn("quest.apk", (sealed / "SHA256SUMS").read_text())
        self.assertEqual(sorted(p.name for p in sealed.iterdir()),
                         ["RELEASE-NOTES.md", "SHA256SUMS", "nevr-runtime-v4.0.0-rc.3-windows.zip"])
        notes = (sealed / "RELEASE-NOTES.md").read_text()
        self.assertIn("No Quest APK in this candidate", notes)
        self.assertNotIn("adb install", notes)
        self.assertNotIn("Two artifacts", notes)
        with zipfile.ZipFile(sealed / "nevr-runtime-v4.0.0-rc.3-windows.zip") as z:
            signing = z.read("SIGNING.txt").decode()
            self.assertTrue(signing.startswith("UNSIGNED\n"))
            self.assertIn("No Quest APK in this candidate", signing)

    def test_seal_with_an_apk_keeps_the_quest_text(self):
        tree = self.make_tree()
        sealed = self.tmp / "sealed"
        self.assertEqual(self.run_stage("seal", "--tree", str(tree), "--out", str(sealed),
                                        "--apk", str(self.apk)).returncode, 0)
        notes = (sealed / "RELEASE-NOTES.md").read_text()
        self.assertIn("adb install -r nevr-runtime-v4.0.0-rc.3-quest.apk", notes)
        self.assertNotIn("No Quest APK in this candidate", notes)

    def test_seal_refuses_an_attached_apk_that_is_not_stamped_with_this_candidate(self):
        tree = self.make_tree()
        for version in ("4.0.0-dev+1172.3a35e0b9", "4.0.0-rc.4+1172.3a35e0b9", "4.0.0+1172.3a35e0b9"):
            with self.subTest(version=version):
                apk = self.tmp / "tester.apk"
                with zipfile.ZipFile(apk, "w") as z:
                    z.writestr("lib/arm64-v8a/libovrplatformloader.so",
                               b"\x7fELF" + version.encode() + b"\0" + COMMIT[:8].encode())
                sealed = self.tmp / "sealed"
                result = self.run_stage("seal", "--tree", str(tree), "--out", str(sealed), "--apk", str(apk))
                self.assertEqual(result.returncode, 1)
                self.assertIn("carries no '-rc.3+' version string", result.stderr)
                self.assertFalse(sealed.exists())

    def stamp(self, label, dll=None):
        return self.run_stage("stamp", "--dll", str(dll or self.dll), "--label", label)

    def test_stamp_accepts_the_label_the_build_was_asked_for(self):
        result = self.stamp("rc.3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("stamped version: 4.0.0-rc.3+1172.3a35e0b9", result.stdout)

    def test_stamp_refuses_a_candidate_label_the_binary_does_not_carry(self):
        for label in ("rc.4", "rc.03x", "dev"):
            with self.subTest(label=label):
                result = self.stamp(label)
                self.assertEqual(result.returncode, 1)
        self.dll.write_bytes(b"MZ" + b"4.0.0-dev+1172.3a35e0b9\0" + COMMIT[:8].encode())
        result = self.stamp("rc.3")
        self.assertEqual(result.returncode, 1)
        self.assertIn("the tag build did not stamp the candidate", result.stderr)
        self.assertIn("4.0.0-dev+1172.3a35e0b9", result.stderr)

    def test_stamp_without_a_label_refuses_a_binary_carrying_rc(self):
        result = self.stamp("")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no release-candidate label was computed", result.stderr)
        self.dll.write_bytes(b"MZ" + b"4.0.0-dev+1172.3a35e0b9\0")
        self.assertEqual(self.stamp("").returncode, 0)
        self.dll.write_bytes(b"MZ" + b"4.0.0+1172.3a35e0b9\0")
        self.assertEqual(self.stamp("").returncode, 0)


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
