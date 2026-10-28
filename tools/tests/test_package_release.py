"""package_release.py: the gate and the assembly, with fake artifacts; install.ps1/uninstall.ps1 under pwsh.

A release is a version plus GitHub's pre-release flag. The packaging path has NO flag input and no word of
the zip, its SHA256SUMS, the release notes or SIGNING.txt depends on it: promoting a release changes no byte.
A local build is a development build (X.Y.(Z+1)-dev.N+sha) and is packaged under a dev name.
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "package_release.py"
PACKAGE = REPO / "tools" / "package-release"
sys.path.insert(0, str(REPO / "tools"))
import package_release  # noqa: E402
from pe_version_fixtures import shipped_pe  # noqa: E402
import axml_fixtures  # noqa: E402

COMMIT = "3a35e0b945456dba8a9b56e8110df330178e7259"
DEV = "5.0.1-dev.3+3a35e0b"
DEV_VERSION_CODE = 50_001_003  # 5*10^7 + 0*10^5 + 1*10^3 + 3 (tools/quest_apk_version.py)
RELEASE = "5.0.0"
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


def build_info(version):
    return (f'inline constexpr const char* kVersion = "{version}";\n'
            f'inline constexpr const char* kCommit = "{COMMIT[:8]}";\n')


def identity(version, commit=COMMIT):
    return f"NEVR-BUILD {version} {commit}".encode()


def binary(version, commit=COMMIT, literals=1, extra=b"", trust=(b"NEVR-PLUGIN-TRUST release",)):
    """A fake nevr-runtime.dll: a PE with the pinned VERSIONINFO, then the defaults, the identity literal NUL-bounded
    `literals` times, the plugin-trust literal(s) in `trust` (ADR 0010), then `extra`."""
    parts = [v.encode() for v in VALUES.values()] + [identity(version, commit)] * literals + list(trust)
    return shipped_pe("nevr-runtime.dll", version, b"\0" + b"\0".join(parts) + b"\0" + extra)


def host_binary(version, commit=COMMIT, literals=1, extra=b""):
    """A fake BugSplat64.dll, the thin host: a PE with the pinned VERSIONINFO and the identity literal. It embeds no
    endpoints and carries no plugin-trust literal."""
    parts = [identity(version, commit)] * literals
    return shipped_pe("BugSplat64.dll", version, b"\0" + b"\0".join(parts) + b"\0" + extra)


class PackageFixture(unittest.TestCase):
    """Fake artifacts for the tool. VERSION is what the fake binaries are stamped with."""
    VERSION = DEV

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="package-release-test-"))
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.version = self.VERSION
        self.defaults = self.tmp / "public-defaults.env"
        self.defaults.write_text("".join(f"{k}={v}\n" for k, v in VALUES.items()))
        self.pc_header = self.tmp / "pc.h"
        self.pc_header.write_text(defaults_header(VALUES))
        self.quest_header = self.tmp / "quest.h"
        self.quest_header.write_text(defaults_header(VALUES))
        self.info = self.tmp / "info.h"
        self.info.write_text(build_info(self.version))
        self.dll = self.tmp / "BugSplat64.dll"
        self.dll.write_bytes(host_binary(self.version))
        self.runtime = self.tmp / "nevr-runtime.dll"
        self.runtime.write_bytes(binary(self.version))
        self.server = self.tmp / "echovr_server.exe"
        self.server.write_bytes(shipped_pe("echovr_server.exe", self.version, b"\0the dedicated-server launcher\0"))
        self.apk = self.tmp / "signed.apk"
        self.write_apk(self.version)
        self.out = self.tmp / "out"

    def write_apk(self, version, version_code=DEV_VERSION_CODE, manifest=None):
        sentinel = b"\x7fELF" + b"\0".join([v.encode() for v in VALUES.values()] + [version.encode(), COMMIT[:8].encode()])
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", sentinel)
            z.writestr("AndroidManifest.xml", manifest if manifest is not None else axml_fixtures.build_manifest(version_code))

    def run_tool(self, **overrides):
        """The local path (`just package-dev`): no --version, the binaries must carry a dev version."""
        args = {"--commit": COMMIT, "--out": str(self.out), "--dll": str(self.dll),
                "--runtime-dll": str(self.runtime), "--server-exe": str(self.server), "--apk": str(self.apk), "--pc-header": str(self.pc_header),
                "--quest-header": str(self.quest_header), "--quest-build-info": str(self.info),
                "--defaults": str(self.defaults)}
        args.update(overrides)
        flat = [x for kv in args.items() for x in kv]
        return subprocess.run([sys.executable, "-I", str(TOOL), *flat], capture_output=True, text=True)


def words_of_the_old_model(text):
    return re.findall(r"(?i)release candidate|\bcandidate\b|\brc\b|-rc\b|\brc\.\d", text)


class LocalDevPackageTest(PackageFixture):
    """The local build: a development version, dev-named files, never a release."""

    def test_a_clean_local_build_is_packaged_under_a_dev_name_with_checksums_and_notes(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        zip_path = self.out / "nevr-runtime-v5.0.1-dev.3-3a35e0b-windows.zip"
        apk_path = self.out / "nevr-runtime-v5.0.1-dev.3-3a35e0b-quest.apk"
        self.assertTrue(zip_path.exists() and apk_path.exists(), sorted(p.name for p in self.out.iterdir()))
        with zipfile.ZipFile(zip_path) as z:
            self.assertEqual(sorted(z.namelist()),
                             ["BugSplat64.dll", "README.txt", "SHA256SUMS", "SIGNING.txt", "echovr_server.exe",
                              "install.ps1", "nevr-runtime.dll", "quest-diagnose.ps1", "uninstall.ps1"])
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            for name in z.namelist():
                if name != "SHA256SUMS":
                    self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)
            self.assertIn(self.version, z.read("README.txt").decode())
        top = (self.out / "SHA256SUMS").read_text()
        self.assertIn(hashlib.sha256(zip_path.read_bytes()).hexdigest(), top)
        self.assertIn(hashlib.sha256(apk_path.read_bytes()).hexdigest(), top)
        notes = (self.out / "RELEASE-NOTES.md").read_text()
        for needle in ("install.ps1", "uninstall.ps1", "quest-diagnose.ps1", self.version, zip_path.name, apk_path.name,
                       "adb install"):
            self.assertIn(needle, notes)

    def test_a_local_run_refuses_a_binary_stamped_as_a_release(self):
        self.runtime.write_bytes(binary(RELEASE))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not a development version", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_binary_without_the_identity_literal_is_refused(self):
        blob = b"\0".join(v.encode() for v in VALUES.values())
        self.dll.write_bytes(b"MZ\0" + blob + b"\0" + DEV.encode())  # the version, but not the literal
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected exactly one 'NEVR-BUILD <version> <commit40>' literal, found 0", result.stderr)

    def test_two_identity_literals_are_refused(self):
        self.runtime.write_bytes(binary(self.version, literals=2))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("found 2", result.stderr)

    def test_a_literal_that_is_not_nul_bounded_is_not_the_identity(self):
        self.dll.write_bytes(b"MZ" + b"x" + identity(self.version) + b"y")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("found 0", result.stderr)

    def test_the_wrong_commit_is_refused(self):
        self.runtime.write_bytes(binary(self.version, commit="f" * 40))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the checked-out commit", result.stderr)

    def test_a_local_run_has_no_version_option(self):
        result = self.run_tool(**{"--version": "5.0.0"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments: --version", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_dll_that_embeds_no_endpoints_is_refused_and_nothing_is_written(self):
        self.runtime.write_bytes(b"MZ\0" + identity(self.version) + b"\0")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: NEVR_SOCKET_URI: not embedded in nevr-runtime.dll", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_quest_build_that_needs_a_config_file_to_log_in_is_refused(self):
        # No config.yaml key turns the login on: it follows from the embedded socket URI and server key.
        for key, name, what in (("NEVR_SOCKET_URI", "kSocketUri", "socket URI"),
                                ("NEVR_PUBLIC_SOCKET_KEY", "kPublicSocketKey", "server key")):
            with self.subTest(missing=what):
                emptied = dict(VALUES, **{key: ""})
                self.quest_header.write_text(defaults_header(emptied))
                result = self.run_tool()
                self.assertEqual(result.returncode, 1)
                self.assertIn(f"quest: no embedded {what}: the APK would need a config.yaml to log in", result.stderr)
                self.assertFalse(self.out.exists())

    def test_the_apk_must_carry_the_versionCode_of_this_build(self):
        # the store APK's own code (the repack was skipped or failed to stamp), and the code of another build
        for code, label in ((4_987_566, "the store APK's"), (50_001_004, "another build's"), (50_000_999, "the release before's")):
            with self.subTest(label):
                self.write_apk(self.version, version_code=code)
                result = self.run_tool()
                self.assertEqual(result.returncode, 1)
                # "quest: " is the gate's prefix; the seal's own check (a second defence) has none
                self.assertIn(f"quest: signed.apk: AndroidManifest.xml has versionCode {code}, not {DEV_VERSION_CODE}", result.stderr)
                self.assertFalse(self.out.exists())

    def test_the_seal_refuses_a_foreign_versionCode_on_its_own(self):
        # seal() is the second defence, for an APK that reaches it without the gate
        for code in (4_987_566, 50_001_004):
            with self.subTest(code=code):
                self.write_apk(self.version, version_code=code)
                with self.assertRaisesRegex(package_release.GateFailure, f"has versionCode {code}, not {DEV_VERSION_CODE}"):
                    package_release.check_apk_version(self.apk, self.version, COMMIT)
        self.write_apk(self.version)
        package_release.check_apk_version(self.apk, self.version, COMMIT)

    def test_an_apk_whose_manifest_cannot_be_read_is_refused(self):
        self.write_apk(self.version, manifest=b"<m/>")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("versionCode", result.stderr)
        self.assertFalse(self.out.exists())

    def test_an_apk_with_no_manifest_is_refused(self):
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so",
                       b"\x7fELF" + b"\0".join([v.encode() for v in VALUES.values()] + [self.version.encode(), COMMIT[:8].encode()]))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("AndroidManifest.xml", result.stderr)

    def test_the_apk_sentinel_must_embed_the_values_too(self):
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", b"\x7fELF" + self.version.encode() + COMMIT[:8].encode())
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("quest: NEVR_HTTP_URI: not embedded in libovrplatformloader.so", result.stderr)

    def test_an_existing_artifact_is_never_overwritten(self):
        self.assertEqual(self.run_tool().returncode, 0)
        again = self.run_tool()
        self.assertEqual(again.returncode, 1)
        self.assertIn("exists; not overwritten", again.stderr)


class ReleaseStagesTest(PackageFixture):
    """The CI path: gate and write the tree (what the signer signs), then seal. A release has no APK."""
    VERSION = RELEASE

    def run_stage(self, stage, *args):
        return subprocess.run([sys.executable, "-I", str(TOOL), stage, *args], capture_output=True, text=True)

    def tree_args(self, **overrides):
        args = {"--version": RELEASE, "--commit": COMMIT, "--out": str(self.out), "--dll": str(self.dll),
                "--runtime-dll": str(self.runtime), "--server-exe": str(self.server), "--pc-header": str(self.pc_header),
                "--defaults": str(self.defaults)}
        args.update(overrides)
        return [x for kv in args.items() for x in kv]

    def make_tree(self):
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.out / "nevr-runtime-v5.0.0-windows"

    def seal_to(self, tree, name="sealed", *extra):
        sealed = self.tmp / name
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(sealed), *extra)
        self.assertEqual(result.returncode, 0, result.stderr)
        return sealed

    def test_tree_holds_the_files_the_signer_signs_and_no_checksums_yet(self):
        tree = self.make_tree()
        self.assertEqual(sorted(p.name for p in tree.iterdir()),
                         ["BugSplat64.dll", "README.txt", "echovr_server.exe", "install.ps1", "nevr-runtime.dll", "quest-diagnose.ps1",
                          "uninstall.ps1"])
        info = (self.out / "release.json").read_text()
        self.assertIn('"version": "5.0.0"', info)
        self.assertIn(COMMIT, info)

    def test_the_tree_stage_refuses_a_development_version(self):
        self.runtime.write_bytes(binary(DEV))
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the expected '5.0.0'", result.stderr)
        self.assertFalse(self.out.exists())

    def test_the_tree_stage_refuses_another_release_version_or_commit(self):
        self.runtime.write_bytes(binary("5.0.1"))
        self.assertEqual(self.run_stage("tree", *self.tree_args()).returncode, 1)
        self.runtime.write_bytes(binary(RELEASE, commit="e" * 40))
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the checked-out commit", result.stderr)

    def test_the_tree_stage_refuses_a_host_and_a_runtime_that_are_not_one_build(self):
        self.dll.write_bytes(host_binary(RELEASE, commit="e" * 40))
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("BugSplat64.dll: commit eeeeeeeeeeee is not the checked-out commit", result.stderr)
        self.assertIn("BugSplat64.dll and nevr-runtime.dll carry different identity literals", result.stderr)
        self.assertFalse(self.out.exists())

    def test_the_tree_stage_refuses_a_host_that_carries_the_plugin_trust_literal(self):
        self.dll.write_bytes(host_binary(RELEASE, extra=b"NEVR-PLUGIN-TRUST release\0"))
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("BugSplat64.dll", result.stderr)
        self.assertFalse(self.out.exists())

    def test_the_tree_stage_refuses_a_version_that_is_not_x_y_z(self):
        for bad in ("v5.0.0", "5.0", "5.0.0-rc.1", "5.0.0-dev.1+abc1234", "05.0.0", "5.00.0", "5.0.00", "5.0.0+x"):
            with self.subTest(version=bad):
                result = self.run_stage("tree", *self.tree_args(**{"--version": bad}))
                self.assertEqual(result.returncode, 1)
                self.assertIn("is not X.Y.Z", result.stderr)

    def test_the_tree_stage_refuses_a_dll_that_embeds_no_defaults(self):
        self.runtime.write_bytes(b"MZ\0" + identity(RELEASE) + b"\0")
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: NEVR_SOCKET_URI: not embedded in nevr-runtime.dll", result.stderr)

    def test_the_file_names_follow_the_tag_version(self):
        self.dll.write_bytes(host_binary("5.1.2"))
        self.runtime.write_bytes(binary("5.1.2"))
        self.server.write_bytes(shipped_pe("echovr_server.exe", "5.1.2", b"\0the dedicated-server launcher\0"))
        result = self.run_stage("tree", *self.tree_args(**{"--version": "5.1.2"}))
        self.assertEqual(result.returncode, 0, result.stderr)
        tree = self.out / "nevr-runtime-v5.1.2-windows"
        sealed = self.seal_to(tree)
        self.assertEqual(sorted(p.name for p in sealed.iterdir()), ["nevr-runtime-v5.1.2-windows.zip"])

    QUEST = b"\x7fELF the quest sentinel library"

    def release_dir(self, sealed, name="assets", version="5.0.0", quest=True):
        """What the draft holds: the sealed zip and the Quest library under the names the release gives them."""
        assets = Path(tempfile.mkdtemp(prefix=name + "-", dir=self.tmp))
        shutil.copy(sealed / f"nevr-runtime-v{version}-windows.zip", assets)
        if quest:
            (assets / f"nevr-runtime-v{version}-quest-libovrplatformloader.so").write_bytes(self.QUEST)
        return assets

    def notes_for(self, sealed, name="notes", **kw):
        out = Path(tempfile.mkdtemp(prefix=name + "-", dir=self.tmp)) / "notes.md"
        result = self.run_stage("notes", "--assets", str(self.release_dir(sealed, name + "-assets", **kw)), "--out", str(out))
        self.assertEqual(result.returncode, 0, result.stderr)
        return out

    def test_the_release_body_is_the_notes_and_a_sha256_table_of_both_assets(self):
        sealed = self.seal_to(self.make_tree())
        notes = self.notes_for(sealed).read_text()
        zip_path = sealed / "nevr-runtime-v5.0.0-windows.zip"
        quest = "nevr-runtime-v5.0.0-quest-libovrplatformloader.so"
        self.assertIn(f"| `{zip_path.name}` | `{hashlib.sha256(zip_path.read_bytes()).hexdigest()}` |", notes)
        self.assertIn(f"| `{quest}` | `{hashlib.sha256(self.QUEST).hexdigest()}` |", notes)
        self.assertIn(f"Commit `{COMMIT}`", notes)
        self.assertRegex(notes, r"\A# \w+ runtime 5\.0\.0\n")
        self.assertNotRegex(notes, r"@[A-Z_]+@", "every placeholder is filled")
        self.assertNotIn("SHA256SUMS` | SHA-256 of", notes, "the APK table is not the release's")

    def test_the_body_hashes_the_asset_bytes_as_they_are_so_a_resigned_zip_changes_it(self):
        tree = self.make_tree()
        first = self.notes_for(self.seal_to(tree, "one")).read_text()
        for dll in ("BugSplat64.dll", "nevr-runtime.dll"):
            (tree / dll).write_bytes(fake_pe(0x100, (tree / dll).read_bytes()))
        (tree / "echovr_server.exe").write_bytes(fake_pe(0x100, b"y"))
        self.sign_scripts(tree)
        second = self.notes_for(self.seal_to(tree, "two", "--require-signed")).read_text()
        self.assertNotEqual(first, second)

    def test_the_body_takes_version_and_commit_from_the_dll_inside_the_zip(self):
        sealed = self.seal_to(self.make_tree())
        assets = self.release_dir(sealed, "wrong", version="5.0.0")
        renamed = assets / "nevr-runtime-v5.0.1-windows.zip"
        (assets / "nevr-runtime-v5.0.0-windows.zip").rename(renamed)
        (assets / "nevr-runtime-v5.0.1-quest-libovrplatformloader.so").write_bytes(self.QUEST)
        (assets / "nevr-runtime-v5.0.0-quest-libovrplatformloader.so").unlink()
        result = self.run_stage("notes", "--assets", str(assets), "--out", str(self.tmp / "n.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("carries version '5.0.0', not '5.0.1'", result.stderr)
        self.assertFalse((self.tmp / "n.md").exists())

    def test_the_body_refuses_a_missing_quest_library_a_stray_file_and_an_existing_output(self):
        sealed = self.seal_to(self.make_tree())
        assets = self.release_dir(sealed, "a1", quest=False)
        result = self.run_stage("notes", "--assets", str(assets), "--out", str(self.tmp / "n1.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no nevr-runtime-v5.0.0-quest-libovrplatformloader.so", result.stderr)
        assets = self.release_dir(sealed, "a2")
        (assets / "SHA256SUMS").write_text("x")
        result = self.run_stage("notes", "--assets", str(assets), "--out", str(self.tmp / "n2.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("files that are not release assets: SHA256SUMS", result.stderr)
        (assets / "SHA256SUMS").unlink()
        out = self.tmp / "n3.md"
        out.write_text("kept")
        result = self.run_stage("notes", "--assets", str(assets), "--out", str(out))
        self.assertEqual(result.returncode, 1)
        self.assertIn("exists; not overwritten", result.stderr)
        self.assertEqual(out.read_text(), "kept")

    def test_the_body_refuses_no_zip_two_zips_and_a_zip_without_the_dll(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        result = self.run_stage("notes", "--assets", str(empty), "--out", str(self.tmp / "n.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected exactly one nevr-runtime-v<X.Y.Z>-windows.zip, found 0", result.stderr)
        hollow = self.tmp / "hollow"
        hollow.mkdir()
        with zipfile.ZipFile(hollow / "nevr-runtime-v5.0.0-windows.zip", "w") as z:
            z.writestr("README.txt", "x")
        result = self.run_stage("notes", "--assets", str(hollow), "--out", str(self.tmp / "n.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("has no BugSplat64.dll", result.stderr)

    def sign_scripts(self, tree):
        """What signing does to a PowerShell script: an Authenticode block is appended."""
        for name in ("install.ps1", "uninstall.ps1", "quest-diagnose.ps1"):
            path = tree / name
            path.write_bytes(path.read_bytes() + b"\n# SIG # Begin signature block\n# SIG # End signature block\n")

    def test_a_release_is_the_one_zip_with_no_top_level_sums_notes_or_quest_apk(self):
        tree = self.make_tree()
        sealed = self.seal_to(tree)
        self.assertEqual(sorted(p.name for p in sealed.iterdir()), ["nevr-runtime-v5.0.0-windows.zip"])
        with zipfile.ZipFile(sealed / "nevr-runtime-v5.0.0-windows.zip") as z:
            self.assertEqual(sorted(z.namelist()),
                             ["BugSplat64.dll", "README.txt", "SHA256SUMS", "SIGNING.txt", "echovr_server.exe",
                              "install.ps1", "nevr-runtime.dll", "quest-diagnose.ps1", "uninstall.ps1"])
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            self.assertEqual(sorted(sums), sorted(n for n in z.namelist() if n != "SHA256SUMS"))
            for name in sums:
                self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)

    def test_the_tree_stage_refuses_a_missing_or_non_pe_server_exe(self):
        self.server.write_bytes(b"not a PE")
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: echovr_server.exe is missing or not a PE file", result.stderr)
        self.assertFalse(self.out.exists())
        self.server.unlink()
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("echovr_server.exe is missing or not a PE file", result.stderr)

    def test_the_tree_stage_refuses_a_file_without_the_pinned_version_info(self):
        """#850: the Details tab of both shipped files is part of the gate, at the version the DLL states."""
        self.server.write_bytes(b"MZ\0the dedicated-server launcher\0")
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: echovr_server.exe:", result.stderr)
        self.assertFalse(self.out.exists())
        self.server.write_bytes(shipped_pe("echovr_server.exe", "5.0.1"))
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: echovr_server.exe: FILEVERSION 5.0.1.0 is not 5.0.0.0", result.stderr)
        self.server.write_bytes(shipped_pe("echovr_server.exe", RELEASE))
        parts = [v.encode() for v in VALUES.values()] + [identity(RELEASE), b"NEVR-PLUGIN-TRUST release"]
        self.dll.write_bytes(b"MZ\0" + b"\0".join(parts) + b"\0")  # everything but the VERSIONINFO
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: BugSplat64.dll:", result.stderr)
        self.assertFalse(self.out.exists())

    def test_seal_refuses_a_tree_without_the_server_exe(self):
        tree = self.make_tree()
        (tree / "echovr_server.exe").unlink()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("has no echovr_server.exe", result.stderr)
        self.assertFalse((self.tmp / "s").exists())

    def test_the_texts_state_facts_that_hold_before_and_after_signing_and_use_no_old_model_words(self):
        tree = self.make_tree()
        sealed = self.seal_to(tree)
        with zipfile.ZipFile(sealed / "nevr-runtime-v5.0.0-windows.zip") as z:
            texts = {"SIGNING.txt": z.read("SIGNING.txt").decode(), "README.txt": z.read("README.txt").decode()}
        texts["RELEASE-NOTES.md"] = self.notes_for(sealed).read_text()
        for name, text in texts.items():
            self.assertEqual(words_of_the_old_model(text), [], name)
            self.assertNotRegex(text, r"\bUNSIGNED\b", f"{name} must not state a build-time unsigned fact")
            self.assertNotRegex(text, r"(?i)\b(pre-?release)\b", f"{name} must not depend on the pre-release flag")
        self.assertEqual(texts["SIGNING.txt"], package_release.SIGNING_TEXT)
        self.assertIn("may sign", texts["SIGNING.txt"])
        # quest-diagnose.ps1 is a shipped script: SIGNING.txt names it as signable and as checkable (#750).
        self.assertEqual(texts["SIGNING.txt"].count("quest-diagnose.ps1"), 2)
        # The README section says what the diagnose zip holds and where to send it (#750).
        for needle in ("-File quest-diagnose.ps1", "nevr-diagnose-YYYYMMDDTHHMMSSZ.zip", "crash dumps",
                       "Wi-Fi name", "numeric user ids", "sprockee on the EchoVRCE Discord",
                       "never in a public channel"):
            self.assertIn(needle, texts["README.txt"], needle)
        # No sentence may be false in a package built anywhere: not "built by CI", not "UNSIGNED".
        self.assertNotIn("built by CI", package_release.SIGNING_TEXT)
        self.assertIn("No Quest APK in this release", texts["RELEASE-NOTES.md"])
        self.assertNotIn("adb install", texts["RELEASE-NOTES.md"])
        self.assertNotIn("windows-server", texts["RELEASE-NOTES.md"])
        self.assertNotRegex(texts["RELEASE-NOTES.md"], r"-(lite|debug)\.zip")

    def test_the_zip_readme_tells_an_operator_how_to_start_a_server_and_its_config_uses_keys_the_runtime_reads(self):
        import yaml
        sealed = self.seal_to(self.make_tree())
        with zipfile.ZipFile(sealed / "nevr-runtime-v5.0.0-windows.zip") as z:
            readme = z.read("README.txt").decode()
        section = readme.split("Running a dedicated server\n", 1)[1].split("\nUpdate\n", 1)[0]
        prose = " ".join(section.split())  # the README wraps lines; a phrase may break across one
        for needle in (r".\echovr_server.exe", "-server -headless -no-console", r"_local\config.yaml",
                       "services.socket_uri", "issue #843", "auth.password", "never in the socket_uri",
                       "refresh token", "registration successful", "Graceful shutdown initiated",
                       "services.allow_offline_server"):
            self.assertIn(needle, prose, needle)
        block = [l for l in section.splitlines() if l.startswith("       ") and not l.startswith("       .\\")]
        config = yaml.safe_load("\n".join(l[7:] for l in block))
        paths = {f"{top}.{key}" for top, body in config.items() for key in body}
        self.assertEqual(paths, {"services.socket_uri", "auth.http_uri", "auth.http_key",
                                 "auth.password", "identity.discord_id"})
        mapped = set(re.findall(r'\{"[a-z_]+", "([a-z_]+\.[a-z_]+)"\}',
                                (REPO / "src/runtime/lifecycle/service_map.cpp").read_text(encoding="utf-8")))
        self.assertEqual(paths - mapped, set(), "the README config names a key service_map.cpp does not map")
        self.assertNotRegex(config["services"]["socket_uri"], r"[?&]|@", "no credential rides in the URI")

    def test_the_repository_readme_that_ships_in_the_dist_zips_makes_no_signing_fact(self):
        # README.md is copied into nevr-runtime-vX.Y.Z.zip, which is attested and never
        # change: it must be true whether or not the Windows zip has been signed.
        readme = (REPO / "README.md").read_text(encoding="utf-8")
        self.assertNotRegex(readme, r"(?i)\bunsigned\b|is not signed")
        self.assertEqual(words_of_the_old_model(readme), [])

    def test_seal_regenerates_sha256sums_from_the_signed_files_and_leaves_the_texts_alone(self):
        tree = self.make_tree()
        unsigned_sha = hashlib.sha256((tree / "BugSplat64.dll").read_bytes()).hexdigest()
        before = self.seal_to(tree, "before")
        signed = fake_pe(0x100, (tree / "BugSplat64.dll").read_bytes())  # the signer rewrites the file
        (tree / "BugSplat64.dll").write_bytes(signed)
        signed_runtime = fake_pe(0x100, (tree / "nevr-runtime.dll").read_bytes())
        (tree / "nevr-runtime.dll").write_bytes(signed_runtime)
        signed_server = fake_pe(0x100, (tree / "echovr_server.exe").read_bytes())
        (tree / "echovr_server.exe").write_bytes(signed_server)
        self.sign_scripts(tree)
        sealed = self.seal_to(tree, "after", "--require-signed")
        zip_path = sealed / "nevr-runtime-v5.0.0-windows.zip"
        with zipfile.ZipFile(zip_path) as z, zipfile.ZipFile(before / "nevr-runtime-v5.0.0-windows.zip") as zb:
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            self.assertEqual(sums["BugSplat64.dll"], hashlib.sha256(signed).hexdigest())
            self.assertEqual(sums["nevr-runtime.dll"], hashlib.sha256(signed_runtime).hexdigest())
            self.assertEqual(sums["echovr_server.exe"], hashlib.sha256(signed_server).hexdigest())
            self.assertNotEqual(sums["BugSplat64.dll"], unsigned_sha)
            self.assertEqual(z.read("SIGNING.txt"), zb.read("SIGNING.txt"), "the signing text never changes")
            self.assertEqual(z.read("README.txt"), zb.read("README.txt"))
            self.assertEqual(z.namelist(), zb.namelist(), "same entry names and order")
            for name in z.namelist():
                if name != "SHA256SUMS":
                    self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)
        self.assertEqual(self.notes_for(sealed, "n-after").read_text().replace(hashlib.sha256(zip_path.read_bytes()).hexdigest(), "ZIP"),
                         self.notes_for(before, "n-before").read_text().replace(
                             hashlib.sha256((before / zip_path.name).read_bytes()).hexdigest(), "ZIP"),
                         "the notes differ only in the zip's SHA-256")

    def test_require_signed_is_a_gate_only_and_changes_no_text(self):
        tree = self.make_tree()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"), "--require-signed")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not signed (no certificate table): BugSplat64.dll, echovr_server.exe, nevr-runtime.dll", result.stderr)
        self.assertFalse((self.tmp / "s").exists())
        (tree / "BugSplat64.dll").write_bytes(fake_pe(0x100, b"x"))
        (tree / "nevr-runtime.dll").write_bytes(fake_pe(0x100, b"z"))
        (tree / "echovr_server.exe").write_bytes(fake_pe(0x100, b"y"))
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"), "--require-signed")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not signed (no signature block): install.ps1, quest-diagnose.ps1, uninstall.ps1", result.stderr)

    def test_the_packaging_path_has_no_prerelease_input_and_its_output_is_byte_identical(self):
        # Promoting a release changes only GitHub's flag. The tool has no flag argument at all ...
        tree = self.make_tree()
        for flag in ("--prerelease", "--prerelease=true", "--final"):
            result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "x"), flag)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unrecognized arguments", result.stderr)
        # ... so sealing the same tree twice gives the same bytes, whatever flag the release carries.
        first = self.seal_to(tree, "first")
        second = self.seal_to(tree, "second")
        for name in ("nevr-runtime-v5.0.0-windows.zip",):
            self.assertEqual((first / name).read_bytes(), (second / name).read_bytes(), name)
        self.assertEqual(self.notes_for(first, "n1").read_bytes(), self.notes_for(second, "n2").read_bytes())

    def test_seal_has_no_apk_option(self):
        tree = self.make_tree()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "x"), "--apk", str(self.apk))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments", result.stderr)

    def test_an_existing_release_output_is_never_overwritten(self):
        tree = self.make_tree()
        sealed = self.seal_to(tree)
        self.assertEqual(self.run_stage("seal", "--tree", str(tree), "--out", str(sealed)).returncode, 1)

    # --- the CI assertion after the build -----------------------------------------------------------

    def stamp(self, *args, dll=None):
        return self.run_stage("stamp", "--dll", str(dll or self.runtime), *args)

    def test_stamp_accepts_the_expected_release_version_and_commit(self):
        result = self.stamp("--version", RELEASE, "--commit", COMMIT)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"stamped identity: NEVR-BUILD 5.0.0 {COMMIT}", result.stdout)

    def test_stamp_refuses_a_dev_build_where_a_release_was_expected(self):
        self.runtime.write_bytes(binary(DEV))
        result = self.stamp("--version", RELEASE)
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"version '{DEV}' is not the expected '5.0.0'", result.stderr)

    def test_stamp_without_a_version_wants_a_dev_build_and_refuses_a_release_stamp(self):
        self.assertEqual(self.stamp().returncode, 1)  # the fixture carries 5.0.0: a branch build must not
        self.runtime.write_bytes(binary(DEV))
        self.assertEqual(self.stamp().returncode, 0)

    def test_stamp_checks_the_host_by_its_name_and_wants_no_trust_literal_in_it(self):
        self.assertEqual(self.stamp("--version", RELEASE, "--commit", COMMIT, dll=self.dll).returncode, 0)
        self.dll.write_bytes(host_binary(RELEASE) + b"\0NEVR-PLUGIN-TRUST release\0")
        result = self.stamp("--version", RELEASE, dll=self.dll)
        self.assertEqual(result.returncode, 1)
        self.dll.write_bytes(host_binary(RELEASE, commit="d" * 40))
        self.assertIn("is not the checked-out commit", self.stamp("--version", RELEASE, "--commit", COMMIT, dll=self.dll).stderr)

    def test_stamp_refuses_a_missing_or_doubled_literal_and_the_wrong_commit(self):
        self.dll.write_bytes(b"MZ\0" + b"5.0.0\0")
        self.assertEqual(self.stamp("--version", RELEASE, dll=self.dll).returncode, 1)
        self.runtime.write_bytes(binary(RELEASE, literals=2))
        self.assertIn("found 2", self.stamp("--version", RELEASE).stderr)
        self.runtime.write_bytes(binary(RELEASE))
        result = self.stamp("--version", RELEASE, "--commit", "d" * 40)
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the checked-out commit", result.stderr)

    # --- the Quest library's version and commit (#831) -----------------------------------------------

    def quest_library(self, *strings):
        """A fake Quest library: NUL-bounded strings, as the compiler lays out kVersion and kCommit."""
        path = self.out.parent / "libovrplatformloader.so"
        path.write_bytes(b"\x7fELF\0" + b"\0".join(strings) + b"\0")
        return path

    def quest_check(self, library, version=RELEASE, commit=COMMIT):
        return self.run_stage("quest-library", "--lib", str(library), "--version", version, "--commit", commit)

    def test_quest_library_check_accepts_the_exact_version_and_the_commit_prefix(self):
        library = self.quest_library(b"token_auth_state", RELEASE.encode(), COMMIT[:8].encode(), COMMIT.encode(), b"port")
        result = self.quest_check(library)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"quest library: version {RELEASE} commit {COMMIT[:8]}", result.stdout)
        # git's short hash is 7 digits or more
        for short in (COMMIT[:7], COMMIT[:12], COMMIT[:39]):
            library = self.quest_library(RELEASE.encode(), short.encode(), COMMIT.encode())
            self.assertEqual(self.quest_check(library).returncode, 0, short)

    def test_quest_library_check_refuses_a_dev_version_that_only_starts_with_the_release_version(self):
        library = self.quest_library(b"5.0.0-dev.4", COMMIT[:8].encode(), COMMIT.encode())
        result = self.quest_check(library)
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"no string equal to the version '{RELEASE}'", result.stderr)

    def test_quest_library_check_refuses_a_version_that_is_only_inside_another_string(self):
        library = self.quest_library(b"v5.0.0", b"agent/5.0.0 (x)", COMMIT[:8].encode(), COMMIT.encode())
        result = self.quest_check(library)
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"no string equal to the version '{RELEASE}'", result.stderr)

    def test_quest_library_check_refuses_a_commit_that_is_missing_wrong_or_only_inside_another_string(self):
        full, short, version = COMMIT.encode(), COMMIT[:8].encode(), RELEASE.encode()
        for strings in ((version,),
                        (version, b"d" * 8, b"d" * 40),
                        (version, b"x" + short, b"x" + full),
                        (version, short + b"zz", full + b"zz"),
                        (version, COMMIT[:6].encode(), full),
                        (version, COMMIT[:7].encode() + b"0", full),  # the same first 7 digits, a different 8th
                        (version, short.upper(), full.upper()),
                        (version, full),  # the full commit alone is not kCommit
                        (version, short)):  # kCommit alone, without the build record's commit
            result = self.quest_check(self.quest_library(*strings))
            self.assertEqual(result.returncode, 1, strings)
            self.assertRegex(result.stderr, "no string (of 7 to 39 hex digits that is a prefix of|equal to) the commit")

    def test_quest_library_check_refuses_a_version_argument_that_is_not_x_y_z_and_a_commit_that_is_not_40_hex(self):
        library = self.quest_library(RELEASE.encode(), COMMIT[:8].encode())
        self.assertIn("is not X.Y.Z", self.quest_check(library, version="5.0.0-dev.1").stderr)
        self.assertIn("is not 40 hex", self.quest_check(library, commit=COMMIT[:7]).stderr)

    def test_the_release_workflow_checks_the_quest_library_with_that_command_and_no_bare_grep(self):
        workflow = (REPO / ".github/workflows/build.yml").read_text(encoding="utf-8")
        step = workflow.split("- name: Name and check the release asset", 1)[1].split("\n    - ", 1)[0]
        self.assertIn('python3 tools/package_release.py quest-library --lib "$so" --version "$version" '
                      '--commit "$GITHUB_SHA"', step)
        self.assertNotRegex(step, r"\bgrep\b")

    # --- the plugin signature gate (ADR 0010, #741) --------------------------------------------------

    def test_a_dev_build_is_never_stamped_or_packaged(self):
        self.runtime.write_bytes(binary(RELEASE, trust=(b"NEVR-PLUGIN-TRUST dev",)))
        result = self.stamp("--version", RELEASE)
        self.assertEqual(result.returncode, 1)
        self.assertIn("'dev', not 'release'", result.stderr)
        self.runtime.write_bytes(binary(DEV, trust=(b"NEVR-PLUGIN-TRUST dev",)))
        self.assertEqual(self.stamp().returncode, 1)  # a dev build is refused too: it can be handed to players

    def test_the_literals_the_runtime_used_to_embed_are_not_a_release(self):
        # A DLL built before the run's mode came from -require-signed-plugins says signed-only or unsigned-allowed;
        # neither is what this tool packages.
        for old in (b"signed-only", b"unsigned-allowed"):
            self.runtime.write_bytes(binary(RELEASE, trust=(b"NEVR-PLUGIN-TRUST " + old,)))
            result = self.stamp("--version", RELEASE)
            self.assertEqual(result.returncode, 1, old)
            self.assertIn(f"'{old.decode()}', not 'release'", result.stderr)

    def test_a_binary_without_the_plugin_trust_literal_is_refused_and_so_are_two(self):
        self.runtime.write_bytes(binary(RELEASE, trust=()))
        self.assertIn("found 0", self.stamp("--version", RELEASE).stderr)
        self.runtime.write_bytes(binary(RELEASE, trust=(b"NEVR-PLUGIN-TRUST release",) * 2))
        self.assertIn("found 2", self.stamp("--version", RELEASE).stderr)

    def test_stamp_refuses_a_version_argument_that_is_not_x_y_z(self):
        result = self.stamp("--version", "v5.0.0")
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not X.Y.Z", result.stderr)


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


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class InstallScriptsTest(unittest.TestCase):
    """install.ps1 and uninstall.ps1 run for real under pwsh against a fake install directory."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="package-release-ps-"))
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.pkg = self.tmp / "pkg"
        self.pkg.mkdir()
        self.game = self.tmp / "game" / "bin" / "win10"
        self.game.mkdir(parents=True)
        (self.game / "echovr.exe").write_bytes(b"exe")
        (self.game / "BugSplat64.dll").write_bytes(b"ORIGINAL")
        (self.game / "dbgcore.dll").write_bytes(b"LEGACY")
        # ADR 0012: nevr-runtime.dll goes to the nevr root under the user's %LOCALAPPDATA%, not into the game folder.
        self.appdata = self.tmp / "appdata"
        self.root = self.appdata / "nevr" / "echovr"
        patcher = mock.patch.dict(os.environ, {"LOCALAPPDATA": str(self.appdata)})
        patcher.start()
        self.addCleanup(patcher.stop)
        for name in ("install.ps1", "uninstall.ps1"):
            shutil.copy(PACKAGE / name, self.pkg / name)
        (self.pkg / "BugSplat64.dll").write_bytes(b"NEW-RUNTIME")
        (self.pkg / "nevr-runtime.dll").write_bytes(self.RUNTIME_PAYLOAD)
        self.write_sums()

    RUNTIME_PAYLOAD = b"NEW-RUNTIME-DLL NEVR-BUILD x"

    def write_sums(self):
        """SHA256SUMS of the package's two DLLs as they are now."""
        (self.pkg / "SHA256SUMS").write_text("".join(
            f"{hashlib.sha256((self.pkg / name).read_bytes()).hexdigest()}  {name}\n"
            for name in ("BugSplat64.dll", "nevr-runtime.dll")))

    def ps(self, script, *args):
        return subprocess.run(["pwsh", "-NoProfile", "-File", str(self.pkg / script), *args],
                              capture_output=True, text=True, timeout=60)

    # ADR 0012 / #857: the runtime moved the sign-in cache out of the install's _local folder; uninstalling puts it back.
    def _moved_cache_setup(self, *, local_cache=None, root_cache=b'{"refresh_token":"r-1"}', marker=True):
        appdata = self.tmp / "appdata"
        local = self.tmp / "game" / "_local"  # two parents up from bin\win10: where the stock install keeps it
        local.mkdir(parents=True)
        if marker:
            (local / "MOVED-TO-NEVR-ROOT.txt").write_text("moved\n")
        if local_cache is not None:
            (local / ".credentials.json").write_bytes(local_cache)
        root = appdata / "nevr" / "echovr"
        if root_cache is not None:
            root.mkdir(parents=True)
            (root / ".credentials.json").write_bytes(root_cache)
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
        return local, root

    def uninstall_with_appdata(self):
        env = dict(os.environ, LOCALAPPDATA=str(self.tmp / "appdata"))
        return subprocess.run(["pwsh", "-NoProfile", "-File", str(self.pkg / "uninstall.ps1"), "-Dir", str(self.game)],
                              capture_output=True, text=True, timeout=60, env=env)

    def test_uninstall_puts_the_moved_sign_in_cache_back_and_keeps_the_roots_copy(self):
        local, root = self._moved_cache_setup()
        undone = self.uninstall_with_appdata()
        self.assertEqual(undone.returncode, 0, undone.stdout + undone.stderr)
        self.assertIn("RESTORED the sign-in cache", undone.stdout)
        self.assertEqual((local / ".credentials.json").read_bytes(), b'{"refresh_token":"r-1"}')
        self.assertEqual((root / ".credentials.json").read_bytes(), b'{"refresh_token":"r-1"}', "a later install keeps the login")
        self.assertFalse((local / "MOVED-TO-NEVR-ROOT.txt").exists())
        self.assertEqual(len(list(local.glob("MOVED-TO-NEVR-ROOT.txt.undone-*"))), 1, "set aside, not deleted")

    def test_uninstall_never_overwrites_a_sign_in_cache_that_is_in_local(self):
        local, root = self._moved_cache_setup(local_cache=b"newer")
        undone = self.uninstall_with_appdata()
        self.assertEqual(undone.returncode, 0, undone.stdout + undone.stderr)
        self.assertIn("LEFT", undone.stdout)
        self.assertEqual((local / ".credentials.json").read_bytes(), b"newer")

    def test_uninstall_restores_nothing_into_a_folder_the_runtime_did_not_empty(self):
        local, root = self._moved_cache_setup(marker=False)
        undone = self.uninstall_with_appdata()
        self.assertEqual(undone.returncode, 0, undone.stdout + undone.stderr)
        self.assertFalse((local / ".credentials.json").exists())
        self.assertNotIn("sign-in cache", undone.stdout)

    def test_uninstall_says_so_when_there_is_no_cache_in_the_root_to_put_back(self):
        local, root = self._moved_cache_setup(root_cache=None)
        undone = self.uninstall_with_appdata()
        self.assertEqual(undone.returncode, 0, "the game files are back; this is reported, not fatal")
        self.assertIn("NOT RESTORED the sign-in cache", undone.stdout)
        self.assertIn("there is none in", undone.stdout, "named for what it is, not as a failed copy")
        self.assertEqual(sorted(p.name for p in local.glob(".credentials.json*")), [], "nothing is created for a cache that is not there")
        self.assertTrue((local / "MOVED-TO-NEVR-ROOT.txt").exists(), "the marker stays while the cache is not back")
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

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

    # #759: an update must leave uninstall.ps1 able to put the oldest original back. A runtime DLL carries the
    # NEVR-BUILD identity literal, which install.ps1 uses to tell a runtime from the game's own file and
    # uninstall.ps1 uses to know a DLL is ours. Most tests use a plain payload (no literal) so the hash lists, not
    # the literal, are what they exercise.
    def set_package(self, payload):
        (self.pkg / "BugSplat64.dll").write_bytes(payload)
        self.write_sums()

    @staticmethod
    def runtime(version):
        return b"MZ\0NEVR-BUILD " + version.encode() + b" " + b"a" * 40 + b"\0"

    @staticmethod
    def plain(version):
        return b"MZ\0PLAIN-BUILD " + version.encode() + b"\0"

    def state(self):
        return dict(line.split("=", 1) for line in (self.game / "nevr-install.txt").read_text().splitlines())

    def install_a_then_update_to_b(self):
        self.set_package(self.plain("5.4.3"))
        first = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.b = self.plain("5.4.4")
        self.set_package(self.b)
        return self.ps("install.ps1", "-Dir", str(self.game))

    def output(self, result):
        return result.stdout + result.stderr

    def test_an_update_replaces_the_runtime_keeps_the_oldest_original_and_records_the_new_hash(self):
        updated = self.install_a_then_update_to_b()
        self.assertEqual(updated.returncode, 0, self.output(updated))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), self.b)
        backups = list(self.game.glob("BugSplat64.dll.original-*"))
        self.assertEqual([b.read_bytes() for b in backups], [b"ORIGINAL"], "the runtime is never taken as an original")
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(self.b).hexdigest())
        self.assertNotIn(self.state()["installed_sha256"], self.state()["previous_sha256"].split(","))
        self.assertEqual(len(list(self.game.glob("dbgcore.dll.legacy-*"))), 1)

    def test_uninstall_after_an_update_restores_the_oldest_original_and_the_legacy_file(self):
        self.assertEqual(self.install_a_then_update_to_b().returncode, 0)
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertEqual((self.game / "dbgcore.dll").read_bytes(), b"LEGACY")
        self.assertEqual(len(list(self.game.glob("BugSplat64.dll.original-*"))), 1, "the backup is kept")

    def test_a_dll_copied_in_by_hand_is_undone_by_the_package_that_carries_it(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        b = self.plain("5.4.4")
        (self.game / "BugSplat64.dll").write_bytes(b)
        self.set_package(b)
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_a_sha256sums_line_for_another_file_is_not_a_known_build(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        stranger = self.plain("9.9.9")
        (self.game / "BugSplat64.dll").write_bytes(stranger)
        (self.pkg / "SHA256SUMS").write_text(f"{hashlib.sha256(stranger).hexdigest()}  README.txt\n")
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertNotEqual(undone.returncode, 0)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), stranger)

    def test_a_dll_no_package_and_no_record_knows_and_without_the_runtime_literal_is_refused(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (self.game / "BugSplat64.dll").write_bytes(self.plain("9.9.9"))
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertNotEqual(undone.returncode, 0)
        self.assertIn("is not the one install.ps1 put there", self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), self.plain("9.9.9"))
        self.assertTrue((self.game / "nevr-install.txt").exists())

    def test_a_dll_carrying_the_runtime_literal_is_ours_and_is_undone_whatever_package_runs(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (self.game / "BugSplat64.dll").write_bytes(self.runtime("7.7.7"))  # copied in by hand from a package since lost
        self.set_package(self.plain("5.4.3"))
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_an_earlier_recorded_build_is_still_known_after_an_update(self):
        self.assertEqual(self.install_a_then_update_to_b().returncode, 0)
        (self.game / "BugSplat64.dll").write_bytes(self.plain("5.4.3"))  # the old build put back by hand
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_an_update_refuses_a_dll_it_did_not_put_there_and_changes_nothing(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        stranger = self.plain("9.9.9")
        (self.game / "BugSplat64.dll").write_bytes(stranger)
        self.set_package(self.plain("5.4.4"))
        updated = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(updated.returncode, 0)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), stranger)
        self.assertEqual(len(list(self.game.glob("BugSplat64.dll.original-*"))), 1)
        # uninstall.ps1 cannot undo a DLL that is neither recorded nor ours, so the text must not send the player there
        self.assertNotIn("run uninstall.ps1 first", self.output(updated))

    def test_an_update_over_a_runtime_it_did_not_record_points_at_uninstall_which_accepts_it(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (self.game / "BugSplat64.dll").write_bytes(self.runtime("7.7.7"))
        self.set_package(self.plain("5.4.4"))
        updated = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(updated.returncode, 0)
        self.assertIn("run uninstall.ps1 first", self.output(updated))
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))  # the advice works
        self.assertEqual(undone.returncode, 0, self.output(undone))

    def test_this_packages_dll_copied_in_by_hand_is_recorded_not_refused(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        b = self.plain("5.4.4")
        self.set_package(b)
        (self.game / "BugSplat64.dll").write_bytes(b)
        again = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(again.returncode, 0, self.output(again))
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(b).hexdigest())
        self.assertNotIn(hashlib.sha256(b).hexdigest(), self.state()["previous_sha256"].split(","))
        self.set_package(self.plain("5.4.3"))  # a different package's uninstaller, run later
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_a_failed_update_puts_the_previous_dll_back(self):
        # The package DLL is larger than the file-size limit the second install runs under, so the copy over the
        # installed file fails after it has truncated it; the rollback must restore the previous bytes.
        import resource
        import signal
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        installed = (self.game / "BugSplat64.dll").read_bytes()
        self.set_package(self.plain("5.4.4") + b"x" * (80 * 1024 * 1024))
        limit = 64 * 1024 * 1024

        def limited():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))

        result = subprocess.run(["pwsh", "-NoProfile", "-File", str(self.pkg / "install.ps1"), "-Dir", str(self.game)],
                                capture_output=True, text=True, timeout=60, preexec_fn=limited)
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("update failed", self.output(result))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), installed)
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(installed).hexdigest(), "the record is unchanged")

    # The runtime half of the pair (#853, #855): nevr-runtime.dll is a second file, in the nevr root.
    def limited_install(self, *args):
        import resource
        import signal

        def limited():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (64 * 1024 * 1024, 64 * 1024 * 1024))

        return subprocess.run(["pwsh", "-NoProfile", "-File", str(self.pkg / "install.ps1"), "-Dir", str(self.game), *args],
                              capture_output=True, text=True, timeout=60, preexec_fn=limited)

    def put_in_root(self, data, name="nevr-runtime.dll"):
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / name).write_bytes(data)

    def test_install_puts_the_runtime_in_the_nevr_root_not_in_the_game_folder_and_records_both(self):
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, self.output(installed))
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), self.RUNTIME_PAYLOAD)
        self.assertFalse((self.game / "nevr-runtime.dll").exists(), "the install folder keeps only the host")
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"NEW-RUNTIME")
        self.assertEqual(self.state()["runtime_sha256"], hashlib.sha256(self.RUNTIME_PAYLOAD).hexdigest())
        self.assertEqual(self.state()["runtime_root"], str(self.root))
        self.assertIn("nevr-runtime.dll sha256=", installed.stdout)
        self.assertIn(str(self.root), installed.stdout, "the player is told where the runtime went")

    def test_install_without_a_local_app_data_and_no_root_refuses_and_changes_nothing(self):
        env = {k: v for k, v in os.environ.items() if k != "LOCALAPPDATA"}
        result = subprocess.run(["pwsh", "-NoProfile", "-File", str(self.pkg / "install.ps1"), "-Dir", str(self.game)],
                                capture_output=True, text=True, timeout=60, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LOCALAPPDATA is not set", self.output(result))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertEqual((self.game / "dbgcore.dll").read_bytes(), b"LEGACY")
        self.assertFalse((self.game / "nevr-install.txt").exists())

    def test_a_root_argument_replaces_local_app_data_and_uninstall_finds_it_through_the_record(self):
        elsewhere = self.tmp / "elsewhere"
        installed = self.ps("install.ps1", "-Dir", str(self.game), "-Root", str(elsewhere))
        self.assertEqual(installed.returncode, 0, self.output(installed))
        self.assertEqual((elsewhere / "nevr-runtime.dll").read_bytes(), self.RUNTIME_PAYLOAD)
        self.assertFalse((self.root / "nevr-runtime.dll").exists())
        self.assertEqual(self.state()["runtime_root"], str(elsewhere))
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertFalse((elsewhere / "nevr-runtime.dll").exists(), "removed from the recorded root, not from LOCALAPPDATA's")
        self.assertEqual(len(list(elsewhere.glob("nevr-runtime.dll.uninstalled-*"))), 1)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    # The old layout (ADR 0012, "Updating only the host, and going back"): a release before the split put nevr-runtime.dll
    # beside echovr.exe. The host looks in the root only, so install moves that file aside and puts the runtime in the root.
    def stage_old_layout_runtime(self, data=None):
        data = data if data is not None else self.runtime("5.4.2")
        (self.game / "nevr-runtime.dll").write_bytes(data)
        return data

    def test_install_moves_an_old_layout_runtime_out_of_the_game_folder_and_uninstall_puts_it_back(self):
        old = self.stage_old_layout_runtime()
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, self.output(installed))
        self.assertFalse((self.game / "nevr-runtime.dll").exists(), "nothing in the game folder is named nevr-runtime.dll")
        aside = list(self.game.glob("nevr-runtime.dll.legacy-*"))
        self.assertEqual([a.read_bytes() for a in aside], [old], "set aside whole, never deleted")
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), self.RUNTIME_PAYLOAD)
        self.assertEqual(self.state()["runtime_legacy"], str(aside[0]))
        self.assertEqual(self.state()["runtime_aside"], "", "a runtime of ours beside the host is not a stranger's file")
        self.assertIn("MOVED", installed.stdout)
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertEqual((self.game / "nevr-runtime.dll").read_bytes(), old, "the old layout is back as it was")
        self.assertEqual(list(self.game.glob("nevr-runtime.dll.legacy-*")), [], "renamed back, not copied and left")
        self.assertFalse((self.root / "nevr-runtime.dll").exists())
        self.assertEqual(len(list(self.root.glob("nevr-runtime.dll.uninstalled-*"))), 1)
        self.assertIn("RESTORED nevr-runtime.dll", undone.stdout)

    def test_a_file_named_nevr_runtime_dll_beside_the_host_that_is_not_ours_is_left_alone(self):
        self.stage_old_layout_runtime(b"A-STRANGERS-FILE")
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, self.output(installed))
        self.assertEqual((self.game / "nevr-runtime.dll").read_bytes(), b"A-STRANGERS-FILE")
        self.assertEqual(list(self.game.glob("nevr-runtime.dll.*")), [])
        self.assertEqual(self.state()["runtime_legacy"], "")
        self.assertIn("LEFT", installed.stdout)
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), self.RUNTIME_PAYLOAD)

    def make_pre_split_install(self):
        """An install as the release before the split left it: the runtime beside the host, a record with no root."""
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (self.game / "nevr-runtime.dll").write_bytes((self.root / "nevr-runtime.dll").read_bytes())
        (self.root / "nevr-runtime.dll").unlink()
        record = self.game / "nevr-install.txt"
        record.write_text("".join(line + "\n" for line in record.read_text().splitlines()
                                  if not line.startswith(("runtime_root=", "runtime_legacy="))))

    def test_an_update_over_a_pre_split_install_moves_its_runtime_to_the_root(self):
        self.make_pre_split_install()
        old = (self.game / "nevr-runtime.dll").read_bytes()
        self.set_package(self.plain("5.4.4"))
        updated = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(updated.returncode, 0, self.output(updated))
        self.assertFalse((self.game / "nevr-runtime.dll").exists())
        self.assertEqual([a.read_bytes() for a in self.game.glob("nevr-runtime.dll.legacy-*")], [old])
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), self.RUNTIME_PAYLOAD)
        self.assertEqual(self.state()["runtime_root"], str(self.root))
        self.assertNotEqual(self.state()["runtime_legacy"], "")
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_uninstall_of_a_pre_split_record_sets_aside_the_runtime_beside_the_host(self):
        self.make_pre_split_install()
        (self.game / "nevr-runtime.dll").write_bytes(self.runtime("5.4.3"))
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertFalse((self.game / "nevr-runtime.dll").exists())
        self.assertEqual(len(list(self.game.glob("nevr-runtime.dll.uninstalled-*"))), 1, "a record with no root names the game folder")
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_uninstall_stops_before_changing_anything_when_the_moved_legacy_runtime_is_gone(self):
        self.stage_old_layout_runtime()
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        next(self.game.glob("nevr-runtime.dll.legacy-*")).unlink()
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertNotEqual(undone.returncode, 0)
        self.assertIn("the set-aside nevr-runtime.dll is gone", self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"NEW-RUNTIME", "nothing was restored")
        self.assertTrue((self.root / "nevr-runtime.dll").exists())

    def test_a_failed_install_puts_the_old_layout_runtime_back_where_it_was(self):
        old = self.stage_old_layout_runtime()
        self.set_package(self.plain("5.4.3") + b"x" * (80 * 1024 * 1024))
        result = self.limited_install()
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("install failed", self.output(result))
        self.assertEqual((self.game / "nevr-runtime.dll").read_bytes(), old)
        self.assertEqual(list(self.game.glob("nevr-runtime.dll.legacy-*")), [])
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_an_existing_nevr_runtime_dll_is_set_aside_not_overwritten(self):
        self.put_in_root(b"A-STRANGERS-FILE")
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, self.output(installed))
        aside = list(self.root.glob("nevr-runtime.dll.original-*"))
        self.assertEqual([a.read_bytes() for a in aside], [b"A-STRANGERS-FILE"])
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), self.RUNTIME_PAYLOAD)

    def test_a_package_without_the_runtime_is_refused_and_the_game_is_untouched(self):
        (self.pkg / "nevr-runtime.dll").unlink()
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nevr-runtime.dll is not next to install.ps1", self.output(result))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertEqual((self.game / "dbgcore.dll").read_bytes(), b"LEGACY")
        self.assertFalse((self.root / "nevr-runtime.dll").exists())

    def test_a_runtime_that_does_not_match_sha256sums_is_refused_and_the_game_is_untouched(self):
        (self.pkg / "nevr-runtime.dll").write_bytes(b"TAMPERED")
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nevr-runtime.dll does not match SHA256SUMS", self.output(result))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertFalse((self.root / "nevr-runtime.dll").exists())

    def test_an_update_replaces_the_runtime_too_and_records_its_new_hash(self):
        self.assertEqual(self.install_a_then_update_to_b().returncode, 0)
        self.set_package(self.plain("5.4.5"))
        newer = b"NEWER-RUNTIME-DLL"
        (self.pkg / "nevr-runtime.dll").write_bytes(newer)
        self.write_sums()
        updated = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(updated.returncode, 0, self.output(updated))
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), newer)
        self.assertEqual(self.state()["runtime_sha256"], hashlib.sha256(newer).hexdigest())

    def test_a_failed_update_puts_the_previous_runtime_back_as_well(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        runtime_before = (self.root / "nevr-runtime.dll").read_bytes()
        host_before = (self.game / "BugSplat64.dll").read_bytes()
        (self.pkg / "nevr-runtime.dll").write_bytes(b"NEXT-RUNTIME")
        self.set_package(self.plain("5.4.4") + b"x" * (80 * 1024 * 1024))  # the host copy fails after the runtime went in
        result = self.limited_install()
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("update failed", self.output(result))
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), runtime_before)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), host_before)

    def test_a_failed_first_install_puts_every_original_back_and_sets_the_new_runtime_aside(self):
        self.put_in_root(b"A-STRANGERS-FILE")
        self.set_package(self.plain("5.4.3") + b"x" * (80 * 1024 * 1024))
        result = self.limited_install()
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("install failed", self.output(result))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertEqual((self.game / "dbgcore.dll").read_bytes(), b"LEGACY")
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), b"A-STRANGERS-FILE")
        failed = list(self.root.glob("nevr-runtime.dll.failed-*"))
        self.assertEqual([f.read_bytes() for f in failed], [self.RUNTIME_PAYLOAD], "the new runtime is set aside, never deleted")

    def test_uninstall_sets_the_runtime_aside_and_never_deletes_it(self):
        (self.pkg / "nevr-runtime.dll").write_bytes(self.runtime("5.4.3"))
        self.write_sums()
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertFalse((self.root / "nevr-runtime.dll").exists())
        aside = list(self.root.glob("nevr-runtime.dll.uninstalled-*"))
        self.assertEqual([a.read_bytes() for a in aside], [self.runtime("5.4.3")])
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def stranger_then_install(self, version="5.4.3"):
        """A stranger's nevr-runtime.dll is in the game folder when a package whose runtime is ours is installed."""
        self.put_in_root(b"A-STRANGERS-FILE")
        (self.pkg / "nevr-runtime.dll").write_bytes(self.runtime(version))
        self.write_sums()
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, self.output(installed))

    def test_the_state_file_records_the_runtime_install_set_aside(self):
        self.stranger_then_install()
        aside = list(self.root.glob("nevr-runtime.dll.original-*"))
        self.assertEqual(len(aside), 1)
        self.assertEqual(self.state()["runtime_aside"], str(aside[0]))
        # ... and an install that set nothing aside records the empty name.
        other = self.game.parent / "other-game"
        other.mkdir()
        (other / "BugSplat64.dll").write_bytes(b"ORIGINAL")
        (other / "echovr.exe").write_bytes(b"exe")
        clean = self.ps("install.ps1", "-Dir", str(other), "-Root", str(self.tmp / "other-root"))
        self.assertEqual(clean.returncode, 0, self.output(clean))
        self.assertEqual(dict(line.split("=", 1) for line in (other / "nevr-install.txt").read_text().splitlines())["runtime_aside"], "")

    def test_uninstall_puts_back_the_runtime_file_install_set_aside(self):
        self.stranger_then_install()
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), b"A-STRANGERS-FILE")
        self.assertEqual(list(self.root.glob("nevr-runtime.dll.original-*")), [], "renamed back, not copied and left")
        self.assertEqual([a.read_bytes() for a in self.root.glob("nevr-runtime.dll.uninstalled-*")], [self.runtime("5.4.3")])
        self.assertIn("RESTORED nevr-runtime.dll", undone.stdout)

    def test_an_update_keeps_the_runtime_aside_so_uninstall_still_puts_it_back(self):
        self.stranger_then_install()
        (self.pkg / "nevr-runtime.dll").write_bytes(self.runtime("5.4.4"))
        self.set_package(self.plain("5.4.4"))
        self.write_sums()
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        self.assertNotEqual(self.state()["runtime_aside"], "")
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), b"A-STRANGERS-FILE")

    def test_uninstall_stops_before_changing_anything_when_the_runtime_aside_is_gone(self):
        self.stranger_then_install()
        next(self.root.glob("nevr-runtime.dll.original-*")).unlink()
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertNotEqual(undone.returncode, 0)
        self.assertIn("the set-aside nevr-runtime.dll is gone", self.output(undone))
        self.assertNotEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL", "nothing was restored")
        self.assertTrue((self.game / "nevr-install.txt").exists())

    def test_uninstall_never_overwrites_a_runtime_file_that_is_not_ours_with_the_aside(self):
        self.stranger_then_install()
        self.put_in_root(b"NOT-OURS")
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), b"NOT-OURS")
        self.assertEqual([a.read_bytes() for a in self.root.glob("nevr-runtime.dll.original-*")], [b"A-STRANGERS-FILE"],
                         "the aside stays where it is")
        self.assertIn("KEPT", undone.stdout)

    def test_uninstall_leaves_a_nevr_runtime_dll_that_carries_no_identity_literal(self):
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        self.put_in_root(b"NOT-OURS")
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.root / "nevr-runtime.dll").read_bytes(), b"NOT-OURS")
        self.assertIn("LEFT nevr-runtime.dll", undone.stdout)

    def test_a_runtime_already_in_place_is_never_backed_up_as_the_original(self):
        # nevr-install.txt is gone (deleted by hand, or the runtime was copied in without install.ps1).
        (self.game / "BugSplat64.dll").write_bytes(self.runtime("5.4.3"))
        self.set_package(self.runtime("5.4.4"))
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("NEVR runtime", self.output(result))
        self.assertEqual(list(self.game.glob("BugSplat64.dll.original-*")), [])
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), self.runtime("5.4.3"))
        self.assertTrue((self.game / "dbgcore.dll").exists(), "nothing was set aside")

    # #780: the installed hash is never also listed in previous_sha256. A to B to A to B (updates) and B copied in by
    # hand over a record that lists it as previous are the inputs that put it there when the filter is dropped.
    def test_updating_back_and_forth_never_lists_the_installed_hash_as_previous(self):
        a, b = self.plain("5.4.3"), self.plain("5.4.4")
        for payload in (a, b, a, b):
            self.set_package(payload)
            result = self.ps("install.ps1", "-Dir", str(self.game))
            self.assertEqual(result.returncode, 0, self.output(result))
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(b).hexdigest())
        self.assertEqual(self.state()["previous_sha256"], hashlib.sha256(a).hexdigest())

    def test_a_hand_copied_dll_the_record_lists_as_previous_becomes_the_installed_one_and_leaves_previous(self):
        a, b = self.plain("5.4.3"), self.plain("5.4.4")
        for payload in (a, b):
            self.set_package(payload)
            self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        self.set_package(a)  # the older package, its DLL copied in by hand over B
        (self.game / "BugSplat64.dll").write_bytes(a)
        again = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(again.returncode, 0, self.output(again))
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(a).hexdigest())
        self.assertEqual(self.state()["previous_sha256"], hashlib.sha256(b).hexdigest())

    # #779: an install made before #762 may have saved a NEVR runtime as the .original backup. Neither script uses
    # a backup that carries the runtime literal; both say how to get the game's own file back.
    def poison_the_backup(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (backup,) = self.game.glob("BugSplat64.dll.original-*")
        backup.write_bytes(self.runtime("5.0.0"))
        return backup

    def assert_refused_over_a_runtime_backup(self, result, backup):
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("Verify Files", self.output(result))
        self.assertIn("NEVR runtime", self.output(result))
        self.assertEqual(backup.read_bytes(), self.runtime("5.0.0"))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), self.plain("5.4.3"), "nothing was changed")
        self.assertTrue((self.game / "nevr-install.txt").exists(), "the record is kept")

    def test_uninstall_refuses_a_backup_that_is_a_runtime(self):
        backup = self.poison_the_backup()
        self.assert_refused_over_a_runtime_backup(self.ps("uninstall.ps1", "-Dir", str(self.game)), backup)

    def test_an_update_refuses_a_backup_that_is_a_runtime(self):
        backup = self.poison_the_backup()
        self.set_package(self.plain("5.4.4"))
        self.assert_refused_over_a_runtime_backup(self.ps("install.ps1", "-Dir", str(self.game)), backup)

    def test_recording_a_hand_copied_dll_refuses_a_backup_that_is_a_runtime(self):
        backup = self.poison_the_backup()  # installed: 5.4.3
        self.set_package(self.plain("5.4.4"))
        # the record names 5.4.3; this package's 5.4.4 DLL copied in by hand is what install would only record
        (self.game / "BugSplat64.dll").write_bytes(self.plain("5.4.4"))
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("Verify Files", self.output(result))
        self.assertEqual(backup.read_bytes(), self.runtime("5.0.0"))
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(self.plain("5.4.3")).hexdigest(), "nothing was recorded")

    def test_the_uninstall_refusal_ends_at_deleting_the_record_and_install_keeps_its_own_wording(self):
        # Someone uninstalling has the stock file back after Verify Files; install.ps1 would only reinstall the runtime.
        self.poison_the_backup()
        undone = self.output(self.ps("uninstall.ps1", "-Dir", str(self.game)))
        self.assertIn("Verify Files", undone)
        self.assertIn("nevr-install.txt", undone)
        self.assertNotIn("install.ps1 again", undone)
        self.set_package(self.plain("5.4.4"))
        self.assertIn("then run install.ps1 again", self.output(self.ps("install.ps1", "-Dir", str(self.game))))

    # The marker is the exact bytes the build writes (build_identity.cpp): "NEVR-BUILD " with the trailing space,
    # upper case. A backup that only resembles it is not a runtime and is restored.
    def assert_a_backup_with_these_bytes_is_restored(self, payload):
        backup = self.poison_the_backup()
        backup.write_bytes(payload)
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), payload)

    def test_a_backup_with_the_marker_but_no_trailing_space_is_not_a_runtime(self):
        self.assert_a_backup_with_these_bytes_is_restored(b"MZ\0NEVR-BUILD\0" + b"a" * 8)

    def test_a_backup_with_the_marker_in_lower_case_is_not_a_runtime(self):
        self.assert_a_backup_with_these_bytes_is_restored(b"MZ\0nevr-build 5.0.0 " + b"a" * 8)

    # install.ps1 reads the same marker (Test-NevrRuntime): over a backup, and over the BugSplat64.dll it is about to
    # back up. A look-alike is the game's own file to it, not a runtime.
    def assert_install_treats_these_bytes_as_the_games_own_file(self, payload):
        backup = self.poison_the_backup()
        backup.write_bytes(payload)
        self.set_package(self.plain("5.4.4"))
        updated = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(updated.returncode, 0, self.output(updated))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), self.plain("5.4.4"))
        self.assertEqual(backup.read_bytes(), payload, "the backup is kept as it was")
        # a first install over a BugSplat64.dll with these bytes backs it up instead of refusing it as a runtime
        fresh = self.tmp / "fresh" / "bin" / "win10"
        fresh.mkdir(parents=True)
        (fresh / "echovr.exe").write_bytes(b"exe")
        (fresh / "BugSplat64.dll").write_bytes(payload)
        first = self.ps("install.ps1", "-Dir", str(fresh))
        self.assertEqual(first.returncode, 0, self.output(first))
        (saved,) = fresh.glob("BugSplat64.dll.original-*")
        self.assertEqual(saved.read_bytes(), payload)

    def test_install_does_not_count_the_marker_without_the_trailing_space_as_a_runtime(self):
        self.assert_install_treats_these_bytes_as_the_games_own_file(b"MZ\0NEVR-BUILD\0" + b"a" * 8)

    def test_install_does_not_count_the_marker_in_lower_case_as_a_runtime(self):
        self.assert_install_treats_these_bytes_as_the_games_own_file(b"MZ\0nevr-build 5.0.0 " + b"a" * 8)

    # #785: a runtime built before the NEVR-BUILD literal carries the CDN user agent instead, and is a runtime too: an
    # install must not back it up as the game's original, and uninstall must not restore it.
    PRE_MARKER_RUNTIME = b"MZ\0nevr-runtime/1.0\0" + b"a" * 8

    def test_uninstall_refuses_a_pre_marker_runtime_saved_as_the_original(self):
        backup = self.poison_the_backup()
        backup.write_bytes(self.PRE_MARKER_RUNTIME)
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertNotEqual(undone.returncode, 0, self.output(undone))
        self.assertIn("is a NEVR runtime", self.output(undone))
        self.assertEqual(backup.read_bytes(), self.PRE_MARKER_RUNTIME)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), self.plain("5.4.3"), "nothing was changed")

    def test_install_refuses_a_pre_marker_runtime_as_the_games_own_file(self):
        fresh = self.tmp / "fresh-785" / "bin" / "win10"
        fresh.mkdir(parents=True)
        (fresh / "echovr.exe").write_bytes(b"exe")
        (fresh / "BugSplat64.dll").write_bytes(self.PRE_MARKER_RUNTIME)
        self.set_package(self.plain("5.4.4"))
        first = self.ps("install.ps1", "-Dir", str(fresh))
        self.assertNotEqual(first.returncode, 0, self.output(first))
        self.assertIn("is a NEVR runtime", self.output(first))
        self.assertEqual((fresh / "BugSplat64.dll").read_bytes(), self.PRE_MARKER_RUNTIME, "nothing was changed")
        self.assertEqual(list(fresh.glob("BugSplat64.dll.original-*")), [], "no original was backed up")

    def test_an_update_with_the_backup_deleted_says_so_and_changes_nothing(self):
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (backup,) = self.game.glob("BugSplat64.dll.original-*")
        backup.unlink()
        self.set_package(self.plain("5.4.4"))
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("the backup is gone", self.output(result))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), self.plain("5.4.3"))
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(self.plain("5.4.3")).hexdigest())

    def game_files(self):
        return {p.name: p.read_bytes() for p in self.game.iterdir() if p.is_file()}

    def test_a_recorded_hand_copy_with_the_backup_gone_is_refused_and_writes_nothing(self):  # #781
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (backup,) = self.game.glob("BugSplat64.dll.original-*")
        backup.unlink()
        self.set_package(self.plain("5.4.4"))
        (self.game / "BugSplat64.dll").write_bytes(self.plain("5.4.4"))  # the package DLL, copied in by hand
        before = self.game_files()
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("the backup is gone", self.output(result))
        self.assertNotIn("RECORDED", self.output(result))
        self.assertEqual(self.game_files(), before)

    def test_a_recorded_hand_copy_with_a_dbgcore_dll_back_is_refused_and_writes_nothing(self):  # #781
        self.set_package(self.plain("5.4.3"))
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        self.set_package(self.plain("5.4.4"))
        (self.game / "BugSplat64.dll").write_bytes(self.plain("5.4.4"))  # the package DLL, copied in by hand
        (self.game / "dbgcore.dll").write_bytes(b"legacy dbgcore")
        before = self.game_files()
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0, self.output(result))
        self.assertIn("a dbgcore.dll exists again", self.output(result))
        self.assertNotIn("RECORDED", self.output(result))
        self.assertEqual(self.game_files(), before)

    def test_after_verify_files_and_deleting_the_record_install_backs_up_the_games_own_file(self):
        backup = self.poison_the_backup()
        (self.game / "BugSplat64.dll").write_bytes(b"ORIGINAL")  # Verify Files
        (self.game / "nevr-install.txt").unlink()
        moved = backup.with_name("BugSplat64.dll.original-kept")  # a second install in the same second must not collide
        backup.rename(moved)
        installed = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(installed.returncode, 0, self.output(installed))
        fresh = [p for p in self.game.glob("BugSplat64.dll.original-*") if p != moved]
        self.assertEqual([p.read_bytes() for p in fresh], [b"ORIGINAL"])

    def ps_typed(self, typed, script, *args):
        return subprocess.run(["pwsh", "-NoProfile", "-File", str(self.pkg / script), *args],
                              input=typed, capture_output=True, text=True, timeout=60)

    def test_without_dir_a_missing_drive_is_skipped_and_the_folder_is_asked_for(self):
        # #679: the search list names D: and E:; Join-Path on a drive that does not exist threw
        # DriveNotFoundException before anything was looked at.
        result = self.ps_typed(str(self.game) + "\n", "install.ps1")
        self.assertNotIn("DriveNotFound", result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"NEW-RUNTIME")

    def test_a_quoted_answer_naming_the_folder_installs(self):
        result = self.ps_typed('"' + str(self.game) + '"\n', "install.ps1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"NEW-RUNTIME")

    def test_without_dir_an_empty_answer_stops_with_the_usual_message(self):
        result = self.ps_typed("\n", "install.ps1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("DriveNotFound", result.stdout + result.stderr)
        self.assertIn("pass -Dir", result.stdout + result.stderr)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_a_quoted_answer_naming_a_missing_folder_is_refused_cleanly(self):
        result = self.ps_typed('"Z:\\nowhere\\bin\\win10"\n', "install.ps1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("DriveNotFound", result.stdout + result.stderr)
        self.assertIn("no echovr.exe in", result.stdout + result.stderr)

    # #736: a tester who double-clicks the script must see why it stopped. The window closes when the script
    # ends, so the error is printed plainly, appended to nevr-install.log next to the script, and (launched from
    # Explorer, or with -Pause) the script waits for Enter. Input that is redirected never waits.
    def test_an_error_is_printed_plainly_and_logged_next_to_the_script(self):
        missing = str(self.game.parent / "no-such-folder")
        result = self.ps_typed("", "install.ps1", "-Dir", missing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR: no echovr.exe in " + missing, result.stdout)
        log = (self.pkg / "nevr-install.log").read_text()
        self.assertIn("install.ps1", log)
        self.assertIn("ERROR: no echovr.exe in " + missing, log)
        self.assertIn("Dir=" + missing, log)
        self.assertRegex(log, r"\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}")

    def test_an_error_before_the_folder_is_checked_is_logged_with_the_folder_it_was_given(self):
        (self.pkg / "SHA256SUMS").write_text("")
        result = self.ps_typed("", "install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR: SHA256SUMS has no entry for BugSplat64.dll", result.stdout)
        self.assertIn("Dir=" + str(self.game), (self.pkg / "nevr-install.log").read_text())
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_pause_waits_for_enter_after_an_error(self):
        result = self.ps_typed("\n", "install.ps1", "-Dir", str(self.game.parent / "nope"), "-Pause")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Press Enter to close", result.stdout)

    def test_pause_waits_for_enter_after_success_too(self):
        result = self.ps_typed("\n", "install.ps1", "-Dir", str(self.game), "-Pause")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("INSTALLED BugSplat64.dll", result.stdout)
        self.assertIn("Press Enter to close", result.stdout)

    def test_no_pause_never_waits(self):
        result = self.ps_typed("", "install.ps1", "-Dir", str(self.game.parent / "nope"), "-Pause", "-NoPause")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Press Enter to close", result.stdout)

    def test_without_pause_a_script_run_from_a_pipe_does_not_wait(self):
        result = self.ps_typed("", "install.ps1", "-Dir", str(self.game.parent / "nope"))
        self.assertNotIn("Press Enter to close", result.stdout)

    def test_a_success_is_logged_too(self):
        result = self.ps_typed("", "install.ps1", "-Dir", str(self.game))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        log = (self.pkg / "nevr-install.log").read_text()
        self.assertIn("OK install.ps1", log)
        self.assertIn("Dir=" + str(self.game), log)

    def test_uninstall_reports_an_error_the_same_way(self):
        result = self.ps_typed("", "uninstall.ps1", "-Dir", str(self.game))  # nothing installed yet
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR: no nevr-install.txt in " + str(self.game), result.stdout)
        log = (self.pkg / "nevr-install.log").read_text()
        self.assertIn("uninstall.ps1", log)
        self.assertIn("Dir=" + str(self.game), log)

    def test_uninstall_without_dir_asks_for_the_folder_and_pauses_on_request(self):
        self.ps_typed("", "install.ps1", "-Dir", str(self.game))
        result = self.ps_typed(str(self.game) + "\n\n", "uninstall.ps1", "-Pause")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertIn("Press Enter to close", result.stdout)

    def test_uninstall_without_dir_and_an_empty_answer_stops_with_the_usual_message(self):
        result = self.ps_typed("\n", "uninstall.ps1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR: no folder given; pass -Dir", result.stdout)

    def test_pause_in_a_noninteractive_host_does_not_change_the_exit_code(self):
        # Read-Host throws in -NonInteractive; a successful install must still exit 0 and say why it did not wait.
        result = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(self.pkg / "install.ps1"),
                                 "-Dir", str(self.game), "-Pause"],
                                input="", capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("INSTALLED BugSplat64.dll", result.stdout)
        self.assertIn("could not wait for Enter", result.stdout)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"NEW-RUNTIME")

    def test_pause_in_a_noninteractive_host_keeps_an_errors_exit_code(self):
        result = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(self.pkg / "install.ps1"),
                                 "-Dir", str(self.game.parent / "nope"), "-Pause"],
                                input="", capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("ERROR: no echovr.exe in", result.stdout)

    def test_uninstall_pause_in_a_noninteractive_host_does_not_change_the_exit_code(self):
        self.ps_typed("", "install.ps1", "-Dir", str(self.game))
        result = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(self.pkg / "uninstall.ps1"),
                                 "-Dir", str(self.game), "-Pause"],
                                input="", capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")

    def test_an_unwritable_log_does_not_hide_the_error(self):
        (self.pkg / "nevr-install.log").mkdir()  # a directory where the log file should be
        result = self.ps_typed("", "install.ps1", "-Dir", str(self.game.parent / "nope"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR: no echovr.exe in", result.stdout)
        self.assertIn("could not write", result.stdout)

    def test_an_existing_release_output_is_never_overwritten(self):
        (self.pkg / "BugSplat64.dll").write_bytes(b"TAMPERED")
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertTrue((self.game / "dbgcore.dll").exists())

    # #770: Verify Files restores the game's own BugSplat64.dll after an install. The record then describes a file that
    # is no longer there: install.ps1 sets the record aside and installs again, uninstall.ps1 treats the stock file as
    # already restored.
    def test_install_after_verify_files_restored_the_stock_dll_sets_the_stale_record_aside_and_installs(self):
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (self.game / "BugSplat64.dll").write_bytes(b"ORIGINAL")  # Verify Files
        time.sleep(1.1)  # the backup name carries a one-second stamp; the second install must not reuse the first's
        result = self.ps("install.ps1", "-Dir", str(self.game))
        self.assertEqual(result.returncode, 0, self.output(result))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"NEW-RUNTIME")
        stale = list(self.game.glob("nevr-install.txt.stale-*"))
        self.assertEqual(len(stale), 1, self.output(result))
        self.assertIn("installed_sha256=", stale[0].read_text())
        self.assertEqual(self.state()["installed_sha256"], hashlib.sha256(b"NEW-RUNTIME").hexdigest())
        self.assertIn("stale", self.output(result))

    def test_uninstall_after_verify_files_restored_the_stock_dll_leaves_it_and_retires_the_record(self):
        self.assertEqual(self.ps("install.ps1", "-Dir", str(self.game)).returncode, 0)
        (self.game / "BugSplat64.dll").write_bytes(b"ORIGINAL")  # Verify Files
        undone = self.ps("uninstall.ps1", "-Dir", str(self.game))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((self.game / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertFalse((self.game / "nevr-install.txt").exists())
        self.assertEqual(len(list(self.game.glob("nevr-install.txt.undone-*"))), 1)

    # #770: install.ps1 and uninstall.ps1 write nevr-install.txt as UTF-8 without a BOM and read it back as UTF-8, so a
    # folder with a non-ASCII name keeps its backup path. Under -Encoding ASCII the name became '?' and uninstall
    # could not find the backup.
    def test_a_non_ascii_folder_name_round_trips_through_the_state_file(self):
        zoe = self.tmp / "Zoë" / "bin" / "win10"
        zoe.mkdir(parents=True)
        (zoe / "echovr.exe").write_bytes(b"exe")
        (zoe / "BugSplat64.dll").write_bytes(b"ORIGINAL")
        (zoe / "dbgcore.dll").write_bytes(b"LEGACY")
        installed = self.ps("install.ps1", "-Dir", str(zoe))
        self.assertEqual(installed.returncode, 0, self.output(installed))
        raw = (zoe / "nevr-install.txt").read_bytes()
        self.assertFalse(raw.startswith(b"\xef\xbb\xbf"), "the state file is written without a BOM")
        self.assertIn("Zoë".encode("utf-8"), raw)
        undone = self.ps("uninstall.ps1", "-Dir", str(zoe))
        self.assertEqual(undone.returncode, 0, self.output(undone))
        self.assertEqual((zoe / "BugSplat64.dll").read_bytes(), b"ORIGINAL")
        self.assertEqual((zoe / "dbgcore.dll").read_bytes(), b"LEGACY")

    def test_no_script_deletes_anything(self):
        for name in ("install.ps1", "uninstall.ps1"):
            text = (PACKAGE / name).read_text()
            self.assertNotIn("Remove-Item", text, name)
            self.assertNotRegex(text, r"\bdel\b|\brm\b", name)


if __name__ == "__main__":
    unittest.main()
