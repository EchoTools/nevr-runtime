"""package_release.py: the gate and the assembly, with fake artifacts; install.ps1/uninstall.ps1 under pwsh.

A release is a version plus GitHub's pre-release flag. The packaging path has NO flag input and no word of
the zip, its SHA256SUMS, the release notes or SIGNING.txt depends on it: promoting a release changes no byte.
A local build is a development build (<next>-dev.N+sha) and is packaged under a dev name.
"""

import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

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
# The short hash the Quest build compiles into kCommit (git's --short: 9 digits in this repository).
SHORT_COMMIT = COMMIT[:9]
# A commit that is not COMMIT, and a dev version of another build, for the runtime an APK carries.
OTHER_COMMIT = "d" * 40
OTHER_DEV = "5.0.1-dev.4+3a35e0b"
# A 40-hex string the real shim carries that is no commit (a pinned build id; libovrplatformloader.so at a dev build).
SHIM_BUILD_ID = "8c4fddc079eae65909530132a56c48da48b2708c"
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


def host_binary(version, commit=COMMIT, literals=1, extra=b"", build=(b"NEVR-HOST-BUILD release",)):
    """A fake BugSplat64.dll, the thin host: a PE with the pinned VERSIONINFO, the identity literal and the host-build
    literal (#1122). It embeds no endpoints and carries no plugin-trust literal."""
    parts = [identity(version, commit)] * literals + list(build)
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
        self.installer = self.tmp / "install-nevr-runtime.exe"
        self.installer.write_bytes(b"MZ\0the core installer\0")
        # #1492: the same bytes under the name outside callers still use (the packaging step makes it with one cp).
        self.installer_compat = self.tmp / "nevr-install.exe"
        self.installer_compat.write_bytes(self.installer.read_bytes())
        self.apk = self.tmp / "signed.apk"
        self.write_apk(self.version)
        self.out = self.tmp / "out"

    def write_apk(self, version, version_code=DEV_VERSION_CODE, manifest=None, strings=None, runtime=b"",
                  with_runtime=True):
        # The shim as the build writes it: the defaults, the version and the short commit, each NUL-bounded
        # (quest_library_problems matches each whole); no full commit. The runtime it maps carries the full commit in
        # its NEVR-BUILD literal (quest_runtime_problems).
        if strings is None:
            strings = [*VALUES.values(), version, SHORT_COMMIT]
        sentinel = b"\x7fELF" + b"".join(s.encode() + b"\0" for s in strings)
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", sentinel)
            if with_runtime:
                z.writestr("lib/arm64-v8a/libnevr-runtime.so", runtime or b"\x7fELF\0" + identity(version) + b"\0")
            z.writestr("AndroidManifest.xml", manifest if manifest is not None else axml_fixtures.build_manifest(version_code))

    def run_tool(self, **overrides):
        """The local path (`just package-dev`): no --version, the binaries must carry a dev version."""
        args = {"--commit": COMMIT, "--out": str(self.out), "--dll": str(self.dll),
                "--runtime-dll": str(self.runtime), "--server-exe": str(self.server), "--installer-exe": str(self.installer), "--installer-compat-exe": str(self.installer_compat), "--apk": str(self.apk), "--pc-header": str(self.pc_header),
                "--quest-header": str(self.quest_header), "--quest-build-info": str(self.info),
                "--defaults": str(self.defaults)}
        args.update(overrides)
        flat = [x for kv in args.items() for x in kv]
        return subprocess.run([sys.executable, "-I", str(TOOL), *flat], capture_output=True, text=True)


def words_of_the_old_model(text):
    return re.findall(r"(?i)release candidate|\bcandidate\b|\brc\b|-rc\b|\brc\.\d", text)


class LocalDevPackageTest(PackageFixture):
    """The local build: a development version, dev-named files, never a release."""

    def test_the_local_package_refuses_an_old_name_copy_with_other_bytes(self):
        self.installer_compat.write_bytes(b"MZ\0another build of the installer\0")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("nevr-install.exe is not the same bytes as install-nevr-runtime.exe", result.stderr)

    def test_a_clean_local_build_is_packaged_under_a_dev_name_with_checksums_and_notes(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        zip_path = self.out / "nevr-runtime-v5.0.1-dev.3-3a35e0b-windows.zip"
        apk_path = self.out / "nevr-runtime-v5.0.1-dev.3-3a35e0b-quest.apk"
        self.assertTrue(zip_path.exists() and apk_path.exists(), sorted(p.name for p in self.out.iterdir()))
        with zipfile.ZipFile(zip_path) as z:
            self.assertEqual(sorted(z.namelist()),
                             ["BugSplat64.dll", "README.txt", "SHA256SUMS", "SIGNING.txt", "echovr_server.exe",
                              "install-nevr-runtime.exe", "install.ps1", "nevr-install.exe", "nevr-runtime.dll", "quest-diagnose.ps1", "uninstall.ps1"])
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

    def test_the_notes_list_the_files_the_zip_really_holds_and_name_each_signed_file(self):
        """#1492: the tester notes' zip row and signing sentence are the zip's real file set, both installer names included."""
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        zip_path = self.out / "nevr-runtime-v5.0.1-dev.3-3a35e0b-windows.zip"
        with zipfile.ZipFile(zip_path) as z:
            held = set(z.namelist())
        notes = (self.out / "RELEASE-NOTES.md").read_text()
        row = next(line for line in notes.splitlines() if line.startswith(f"| `{zip_path.name}`"))
        self.assertEqual(set(re.findall(r"`([^`]+)`", row.split("|", 3)[2])), held)
        signing = next(line for line in " ".join(notes.split("\n")).split(". ") if "separate signing step may sign" in line)
        signed = {name for name in held if name.endswith((".dll", ".exe", ".ps1"))}
        self.assertEqual(set(re.findall(r"`([^`]+)`", signing)), signed)

    def test_the_template_without_a_quest_apk_lists_the_zips_file_set_too(self):
        values = {"VERSION": "5.0.0", "COMMIT": COMMIT, "ZIP": "z.zip", "APK": "q.apk", "QUEST": "q.so", "SHA_ZIP": "0" * 64,
                  "SHA_QUEST": "1" * 64, "PLAYER_LINE": "x"}
        notes = package_release.render(PACKAGE / "RELEASE-NOTES.template.md", values, apk=False)
        row = next(line for line in notes.splitlines() if line.startswith("| `z.zip`"))
        listed = set(re.findall(r"`([^`]+)`", row.split("|", 4)[3]))
        self.assertEqual(listed, {"BugSplat64.dll", "nevr-runtime.dll", "echovr_server.exe", "install-nevr-runtime.exe",
                                  "nevr-install.exe", "install.ps1", "uninstall.ps1", "quest-diagnose.ps1", "README.txt",
                                  "SIGNING.txt", "SHA256SUMS"})

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

    def test_a_local_gate_refuses_a_quest_library_whose_version_is_only_inside_another_string(self):
        """#863: a bare substring match passes this library; the release job's whole-string check refuses it."""
        inside = f"runtime-{self.version}-build"
        self.write_apk(self.version, strings=[*VALUES.values(), inside, SHORT_COMMIT])
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"quest: quest library: no string equal to the version '{self.version}'", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_local_gate_refuses_a_quest_library_whose_commit_is_only_inside_another_string(self):
        self.write_apk(self.version, strings=[*VALUES.values(), self.version, f"x{SHORT_COMMIT}"])
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("quest: quest library: no string of 7 to 39 hex digits that is a prefix of the commit", result.stderr)

    def test_a_local_gate_refuses_an_apk_whose_runtime_is_missing_or_names_another_build(self):
        """The full commit is the runtime's (libnevr-runtime.so's NEVR-BUILD literal), so the APK's runtime is checked whole."""
        for runtime, with_runtime, expected in (
                (b"", False, "quest: the APK has no lib/arm64-v8a/libnevr-runtime.so"),
                (b"\x7fELF\0" + identity(self.version, OTHER_COMMIT) + b"\0", True, "quest runtime: commit dddd"),
                (b"\x7fELF\0" + identity(OTHER_DEV) + b"\0", True, f"quest runtime: version '{OTHER_DEV}'"),
                (b"\x7fELF\0" + COMMIT.encode() + b"\0", True, "quest runtime: expected exactly one 'NEVR-BUILD")):
            with self.subTest(expected=expected):
                self.write_apk(self.version, runtime=runtime, with_runtime=with_runtime)
                result = self.run_tool()
                self.assertEqual(result.returncode, 1)
                self.assertIn(expected, result.stderr)
                self.assertFalse(self.out.exists())

    def test_a_tester_apk_check_refuses_a_library_whose_version_is_only_inside_another_string(self):
        inside = f"runtime-{self.version}-build"
        self.write_apk(self.version, strings=[*VALUES.values(), inside, SHORT_COMMIT])
        with self.assertRaisesRegex(package_release.GateFailure, re.escape(f"no string equal to the version '{self.version}'")):
            package_release.check_apk_version(self.apk, self.version, COMMIT)

    def test_a_tester_apk_check_refuses_a_library_whose_commit_is_only_inside_another_string(self):
        self.write_apk(self.version, strings=[*VALUES.values(), self.version, f"x{SHORT_COMMIT}"])
        with self.assertRaisesRegex(package_release.GateFailure, "no string of 7 to 39 hex digits that is a prefix of the commit"):
            package_release.check_apk_version(self.apk, self.version, COMMIT)

    def test_a_tester_apk_check_refuses_an_apk_whose_runtime_is_missing_or_names_another_commit(self):
        self.write_apk(self.version, with_runtime=False)
        with self.assertRaisesRegex(package_release.GateFailure, re.escape("signed.apk: the APK has no lib/arm64-v8a/libnevr-runtime.so")):
            package_release.check_apk_version(self.apk, self.version, COMMIT)
        self.write_apk(self.version, runtime=b"\x7fELF\0" + identity(self.version, OTHER_COMMIT) + b"\0")
        with self.assertRaisesRegex(package_release.GateFailure, "quest runtime: commit d+ is not the checked-out commit"):
            package_release.check_apk_version(self.apk, self.version, COMMIT)

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
                "--runtime-dll": str(self.runtime), "--server-exe": str(self.server), "--installer-exe": str(self.installer), "--installer-compat-exe": str(self.installer_compat), "--pc-header": str(self.pc_header),
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
                         ["BugSplat64.dll", "README.txt", "echovr_server.exe", "install-nevr-runtime.exe", "install.ps1", "nevr-install.exe",
                          "nevr-runtime.dll", "quest-diagnose.ps1", "uninstall.ps1"])
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
            (assets / f"nevr-runtime-v{version}-quest-runtime.so").write_bytes(self.QUEST)
        return assets

    CHANGES = ("feat(runtime): a player feature", "fix(quest): a player fix", "fix(arena): a host fix", "docs: a note")

    def range_args(self, changes=None):
        """A git range holding `changes` (newest first, as git log prints them) above a base tag: what the body's player
        line is read from. Returns the --repo, --base and --rev arguments of `notes`."""
        repo = Path(tempfile.mkdtemp(prefix="range-", dir=self.tmp))
        git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false",
               "-c", "tag.gpgSign=false"]
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "chore: base"], check=True)
        subprocess.run(git + ["tag", "base"], check=True)
        for message in reversed(self.CHANGES if changes is None else changes):
            subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", message], check=True)
        subprocess.run(git + ["tag", "rev"], check=True)
        return ["--repo", str(repo), "--base", "base", "--rev", "rev"]

    def run_notes(self, *args, changes=None):
        return self.run_stage("notes", *args, *self.range_args(changes))

    def notes_for(self, sealed, name="notes", changes=None, **kw):
        out = Path(tempfile.mkdtemp(prefix=name + "-", dir=self.tmp)) / "notes.md"
        result = self.run_notes("--assets", str(self.release_dir(sealed, name + "-assets", **kw)), "--out", str(out),
                                changes=changes)
        self.assertEqual(result.returncode, 0, result.stderr)
        return out

    def test_the_release_body_is_a_header_a_player_line_and_one_line_per_file(self):
        sealed = self.seal_to(self.make_tree())
        body = self.notes_for(sealed).read_text()
        self.assertEqual(body.splitlines(), [
            "# nEVR 5.0.0",
            "",
            "Fixes and internal changes.",
            "",
            "- `nevr-runtime-v5.0.0-windows.zip`: install on PC (unzip, close the game, run `install.ps1`); also the "
            "dedicated server (`echovr_server.exe`) and `uninstall.ps1`.",
            "- `nevr-runtime-v5.0.0-quest.apk`: install on Quest (published on the release host, not on this page).",
            "- `nevr-runtime-v5.0.0-quest-libovrplatformloader.so`: for the Quest patcher; players do not need it.",
            "- `nevr-runtime-v5.0.0-quest-runtime.so`: the Quest runtime library, for the Quest's own update; players do not need it.",
            "- `install-nevr-runtime.exe` (inside the zip): the core installer the scripts run.",
        ])

    def test_notes_refuses_a_release_without_the_quest_runtime_library(self):
        sealed = self.seal_to(self.make_tree())
        assets = self.release_dir(sealed, "no-runtime-assets")
        (assets / "nevr-runtime-v5.0.0-quest-runtime.so").unlink()
        out = Path(tempfile.mkdtemp(prefix="no-runtime-", dir=self.tmp)) / "notes.md"
        result = self.run_notes("--assets", str(assets), "--out", str(out))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("nevr-runtime-v5.0.0-quest-runtime.so", result.stderr)
        self.assertFalse(out.exists())

    def test_the_release_body_has_no_hash_no_commit_and_no_table(self):
        sealed = self.seal_to(self.make_tree())
        zip_path = sealed / "nevr-runtime-v5.0.0-windows.zip"
        body = self.notes_for(sealed).read_text()
        self.assertNotRegex(body, r"[0-9a-f]{64}", "no SHA-256")
        self.assertNotRegex(body, r"[0-9a-f]{40}", "no commit")
        self.assertNotIn(hashlib.sha256(zip_path.read_bytes()).hexdigest(), body)
        self.assertNotIn("Commit", body)
        self.assertNotRegex(body, r"(?m)^\|", "no table row")
        self.assertNotRegex(body, r"@[A-Z_]+@", "every placeholder is filled")
        self.assertNotIn("No Quest APK", body)

    def test_the_player_line_is_the_fixed_sentence_whatever_feat_commits_the_range_holds(self):
        sealed = self.seal_to(self.make_tree())
        many = ("feat(runtime): one", "fix(quest): two", "feat(runtime): three", "feat(runtime): four",
                "fix(quest): five")
        self.assertEqual(self.notes_for(sealed, "many", changes=many).read_text().splitlines()[2],
                         "Fixes and internal changes.")
        none = ("fix(arena): a host fix", "feat(plugin): an api", "chore: tidy")
        self.assertEqual(self.notes_for(sealed, "none", changes=none).read_text().splitlines()[2],
                         "Fixes and internal changes.")

    AUTO_LINE = "Fixes and internal changes."  # the default change below is a player feat; it must not show
    DIR = "release-headlines/"
    LEGACY = "release-headline.txt"

    def headline_repo(self, base_files, rev_files, changes=("feat(runtime): a player feature",)):
        """A repository with a base tag and a rev tag over `changes`. `base_files` is the tree at the base, `rev_files` the
        tree at the rev; each maps a path to its text, or to None for a file that is absent at that ref."""
        repo = Path(tempfile.mkdtemp(prefix="headline-", dir=self.tmp))
        git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false",
               "-c", "tag.gpgSign=false"]

        def tree(files):
            for name, text in files.items():
                path = repo / name
                if text is None:
                    path.unlink(missing_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(text, encoding="utf-8")
            subprocess.run(git + ["add", "-A"], check=True)

        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        tree(base_files)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "chore: base"], check=True)
        subprocess.run(git + ["tag", "base"], check=True)
        for message in changes:
            subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", message], check=True)
        tree(rev_files)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "chore: headline"], check=True)
        subprocess.run(git + ["tag", "rev"], check=True)
        return repo

    def headline_notes(self, base_files, rev_files, changes=("feat(runtime): a player feature",)):
        """Runs `notes` over a headline repository. Returns (the process, the body path)."""
        if not hasattr(self, "_headline_sealed"):
            self._headline_sealed = self.seal_to(self.make_tree(), "headline-sealed")
        sealed = self._headline_sealed
        repo = self.headline_repo(base_files, rev_files, changes)
        out = Path(tempfile.mkdtemp(prefix="headline-notes-", dir=self.tmp)) / "notes.md"
        result = self.run_stage("notes", "--assets", str(self.release_dir(sealed, "headline-assets")), "--out", str(out),
                                "--repo", str(repo), "--base", "base", "--rev", "rev")
        return result, out

    def headline_line(self, base_files, rev_files, changes=("feat(runtime): a player feature",)):
        result, out = self.headline_notes(base_files, rev_files, changes)
        self.assertEqual(result.returncode, 0, result.stderr)
        return out.read_text().splitlines()[2]

    def added(self, text, name="a.txt"):
        """The player line when the range adds one headline file `name` holding `text`."""
        return self.headline_line({}, {self.DIR + name: text})

    def refused(self, rev_files, base_files=None):
        """Runs `notes`, expects the refusal (exit 1, no body written), and returns its stderr."""
        result, out = self.headline_notes(base_files or {}, rev_files)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertFalse(out.exists(), "no body is written for a bad headline")
        return result.stderr

    def test_an_added_headline_replaces_the_player_line_and_nothing_else(self):
        result, changed = self.headline_notes({}, {self.DIR + "a.txt": "New headline.\n"})
        self.assertEqual(result.returncode, 0, result.stderr)
        result, auto = self.headline_notes({}, {})
        self.assertEqual(result.returncode, 0, result.stderr)
        changed_lines, auto_lines = changed.read_text().splitlines(), auto.read_text().splitlines()
        self.assertEqual(changed_lines[2], "New headline.")
        self.assertEqual(auto_lines[2], self.AUTO_LINE)
        self.assertEqual(changed_lines[:2] + changed_lines[3:], auto_lines[:2] + auto_lines[3:])

    def test_a_headline_file_that_existed_at_the_base_is_old_and_the_fixed_sentence_is_used(self):
        files = {self.DIR + "a.txt": "Old headline.\n"}
        self.assertEqual(self.headline_line(files, files), self.AUTO_LINE)

    def test_an_old_headline_file_edited_in_the_range_is_still_old(self):
        self.assertEqual(self.headline_line({self.DIR + "a.txt": "Old headline.\n"},
                                            {self.DIR + "a.txt": "Edited headline.\n"}), self.AUTO_LINE)

    def test_no_headline_files_give_the_fixed_sentence(self):
        self.assertEqual(self.headline_line({}, {}), self.AUTO_LINE)

    def test_a_headline_file_deleted_before_the_rev_gives_the_fixed_sentence(self):
        self.assertEqual(self.headline_line({self.DIR + "a.txt": "Old headline.\n"}, {self.DIR + "a.txt": None}),
                         self.AUTO_LINE)

    def test_only_the_file_the_range_adds_is_used_when_an_old_one_exists_at_the_base(self):
        self.assertEqual(self.headline_line({self.DIR + "a.txt": "Old.\n"},
                                            {self.DIR + "a.txt": "Old.\n", self.DIR + "b.txt": "New.\n"}), "New.")

    def test_added_files_are_joined_in_file_name_order(self):
        # Written in the opposite order of their names, so the order is the sort and not the write order.
        self.assertEqual(self.headline_line({}, {self.DIR + "b-second.txt": "Second one.\n",
                                                 self.DIR + "a-first.txt": "First one.\n",
                                                 self.DIR + "c-third.txt": "Third one\n"}),
                         "First one. Second one. Third one.")

    def test_a_file_in_a_subdirectory_is_refused_and_named(self):
        self.assertIn(self.DIR + "sub/a.txt", self.refused({self.DIR + "sub/a.txt": "Nested.\n"}))

    def test_a_file_that_is_not_a_txt_name_is_refused_and_named(self):
        for name in ("notes.md", "a b.txt", "a.TXT", "a.txt.bak", ".txt", "readme.md", ".hidden.txt", "-a.txt", "_a.txt"):
            with self.subTest(name=name):
                self.assertIn(self.DIR + name, self.refused({self.DIR + name: "A line.\n"}))

    def test_a_name_starting_with_a_digit_or_letter_is_a_headline(self):
        for name in ("1482-topic.txt", "Topic.txt", "a.b_c-d.txt"):
            with self.subTest(name=name):
                self.assertEqual(self.added("Fine.\n", name), "Fine.")

    def test_the_readme_is_never_a_headline(self):
        self.assertEqual(self.headline_line({}, {self.DIR + "README.md": "How this works.\n"}), self.AUTO_LINE)
        self.assertEqual(self.headline_line({}, {self.DIR + "README.md": "How this works.\n", self.DIR + "a.txt": "Real.\n"}),
                         "Real.")

    def test_a_misnamed_file_that_existed_at_the_base_is_not_read(self):
        files = {self.DIR + "notes.md": "Old.\n"}
        self.assertEqual(self.headline_line(files, files), self.AUTO_LINE)

    def test_the_legacy_single_file_at_the_rev_is_refused_and_names_the_directory(self):
        for base_files in ({}, {self.LEGACY: "Old.\n"}):
            with self.subTest(base=sorted(base_files)):
                stderr = self.refused({self.LEGACY: "Any text.\n"}, base_files)
                self.assertIn(self.LEGACY, stderr)
                self.assertIn("release-headlines/", stderr)

    def test_the_legacy_single_file_deleted_before_the_rev_is_fine(self):
        self.assertEqual(self.headline_line({self.LEGACY: "Old.\n"}, {self.LEGACY: None}), self.AUTO_LINE)

    def test_an_empty_headline_fails_loudly_and_names_the_file(self):
        stderr = self.refused({self.DIR + "empty.txt": "  \n\t\n"})
        self.assertIn(self.DIR + "empty.txt", stderr)
        self.assertIn("non-empty", stderr)

    def test_an_over_long_headline_fails_loudly_and_names_the_file(self):
        stderr = self.refused({self.DIR + "long.txt": "x" * 201 + "\n"})
        self.assertIn(self.DIR + "long.txt", stderr)
        self.assertIn("200", stderr)

    def test_a_headline_of_exactly_200_characters_is_used(self):
        text = "y" * 199 + "!"
        self.assertEqual(self.added(text + "\n"), text)

    def test_a_headline_with_more_than_one_line_is_refused_and_named(self):
        stderr = self.refused({self.DIR + "two.txt": "First line.\nSecond line.\n"})
        self.assertIn(self.DIR + "two.txt", stderr)
        self.assertIn("one line", stderr)

    def test_blank_lines_around_the_one_line_are_dropped(self):
        self.assertEqual(self.added("\n\n   \n  Real headline  \n\n \t\n"), "Real headline.")

    def test_a_joined_line_of_exactly_600_characters_is_used(self):
        files = {self.DIR + "a.txt": "a" * 199 + ".\n", self.DIR + "b.txt": "b" * 199 + ".\n",
                 self.DIR + "c.txt": "c" * 197 + ".\n"}
        self.assertEqual(len(self.headline_line({}, files)), 600)

    def test_a_joined_line_over_600_characters_is_refused_and_says_so(self):
        files = {self.DIR + "a.txt": "a" * 199 + ".\n", self.DIR + "b.txt": "b" * 199 + ".\n",
                 self.DIR + "c.txt": "c" * 198 + ".\n"}
        stderr = self.refused(files)
        self.assertIn("the joined headline", stderr)
        self.assertIn("600", stderr)
        for name in ("a.txt", "b.txt", "c.txt"):
            self.assertIn(self.DIR + name, stderr)

    def test_a_full_stop_is_added_only_when_the_line_has_no_terminal_punctuation(self):
        self.assertEqual(self.added("Ships it!\n"), "Ships it!")
        self.assertEqual(self.added("Is it?\n"), "Is it?")
        self.assertEqual(self.added("Ends here.\n"), "Ends here.")
        self.assertEqual(self.added("Quest: asks for clocks\n"), "Quest: asks for clocks.")

    def test_a_byte_order_mark_is_not_part_of_the_headline(self):
        self.assertEqual(self.added("﻿With a BOM\n"), "With a BOM.")

    def test_a_headline_that_is_not_utf8_is_refused_and_named(self):
        repo = self.headline_repo({}, {})
        git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false", "-c", "tag.gpgSign=false"]
        (repo / "release-headlines").mkdir()
        (repo / "release-headlines" / "bad.txt").write_bytes(b"\xff\xfe bad\n")
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "chore: bad"], check=True)
        subprocess.run(git + ["tag", "-f", "rev"], check=True)
        sealed = self.seal_to(self.make_tree(), "bad-sealed")
        out = Path(tempfile.mkdtemp(prefix="headline-notes-", dir=self.tmp)) / "notes.md"
        result = self.run_stage("notes", "--assets", str(self.release_dir(sealed, "bad-assets")), "--out", str(out),
                                "--repo", str(repo), "--base", "base", "--rev", "rev")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("release-headlines/bad.txt", result.stderr)
        self.assertIn("UTF-8", result.stderr)

    def record(self, result):
        """The one `headlines` record on stderr, parsed."""
        records = [json.loads(line) for line in result.stderr.splitlines() if line.startswith("{")]
        self.assertEqual(len(records), 1, result.stderr)
        return records[0]

    def test_the_cut_records_the_files_it_used_and_the_line_on_stderr(self):
        result, _ = self.headline_notes({self.DIR + "a.txt": "Old.\n"}, {self.DIR + "a.txt": "Old.\n",
                                                                          self.DIR + "c.txt": "Third\n",
                                                                          self.DIR + "b.txt": "Second.\n"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.record(result), {"tool": "package_release", "event": "headlines",
                                               "used": [self.DIR + "b.txt", self.DIR + "c.txt"],
                                               "line": "Second. Third.", "base": "base", "rev": "rev"})

    def test_the_record_has_an_empty_used_list_when_the_fixed_sentence_is_used(self):
        result, _ = self.headline_notes({self.DIR + "a.txt": "Old.\n"}, {self.DIR + "a.txt": "Old.\n"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.record(result), {"tool": "package_release", "event": "headlines", "used": [],
                                               "line": self.AUTO_LINE, "base": "base", "rev": "rev"})

    def test_a_refused_headline_writes_no_record(self):
        result, _ = self.headline_notes({}, {self.DIR + "empty.txt": "\n"})
        self.assertEqual(result.returncode, 1)
        self.assertNotIn('"event": "headlines"', result.stderr)
        self.assertNotIn('"event":"headlines"', result.stderr)

    def test_a_placeholder_in_the_player_line_is_not_expanded_by_the_template(self):
        template = Path(self.tmp) / "t.md"
        template.write_text("@PLAYER_LINE@\n@ZIP@ @QUEST@\n", encoding="utf-8")
        body = package_release.render(template, {"PLAYER_LINE": "@ZIP@ and @QUEST@", "ZIP": "z.zip", "QUEST": "q.so"})
        self.assertEqual(body, "@ZIP@ and @QUEST@\nz.zip q.so\n")

    def test_the_body_does_not_depend_on_the_zip_bytes(self):
        tree = self.make_tree()
        first = self.notes_for(self.seal_to(tree, "one")).read_text()
        for dll in ("BugSplat64.dll", "nevr-runtime.dll"):
            (tree / dll).write_bytes(fake_pe(0x100, (tree / dll).read_bytes()))
        (tree / "echovr_server.exe").write_bytes(fake_pe(0x100, b"y"))
        (tree / "install-nevr-runtime.exe").write_bytes(fake_pe(0x100, b"i"))
        (tree / "nevr-install.exe").write_bytes(fake_pe(0x100, b"j"))
        self.sign_scripts(tree)
        second = self.notes_for(self.seal_to(tree, "two", "--require-signed")).read_text()
        self.assertEqual(first, second, "the body names files and says what they are for; it holds no hash")

    def test_the_body_takes_version_and_commit_from_the_dll_inside_the_zip(self):
        sealed = self.seal_to(self.make_tree())
        assets = self.release_dir(sealed, "wrong", version="5.0.0")
        renamed = assets / "nevr-runtime-v5.0.1-windows.zip"
        (assets / "nevr-runtime-v5.0.0-windows.zip").rename(renamed)
        (assets / "nevr-runtime-v5.0.1-quest-libovrplatformloader.so").write_bytes(self.QUEST)
        (assets / "nevr-runtime-v5.0.0-quest-libovrplatformloader.so").unlink()
        result = self.run_notes("--assets", str(assets), "--out", str(self.tmp / "n.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("carries version '5.0.0', not '5.0.1'", result.stderr)
        self.assertFalse((self.tmp / "n.md").exists())

    def test_the_body_refuses_a_missing_quest_library_a_stray_file_and_an_existing_output(self):
        sealed = self.seal_to(self.make_tree())
        assets = self.release_dir(sealed, "a1", quest=False)
        result = self.run_notes("--assets", str(assets), "--out", str(self.tmp / "n1.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no nevr-runtime-v5.0.0-quest-libovrplatformloader.so", result.stderr)
        assets = self.release_dir(sealed, "a2")
        (assets / "SHA256SUMS").write_text("x")
        result = self.run_notes("--assets", str(assets), "--out", str(self.tmp / "n2.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("files that are not release assets: SHA256SUMS", result.stderr)
        (assets / "SHA256SUMS").unlink()
        out = self.tmp / "n3.md"
        out.write_text("kept")
        result = self.run_notes("--assets", str(assets), "--out", str(out))
        self.assertEqual(result.returncode, 1)
        self.assertIn("exists; not overwritten", result.stderr)
        self.assertEqual(out.read_text(), "kept")

    def test_the_body_refuses_no_zip_two_zips_and_a_zip_without_the_dll(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        result = self.run_notes("--assets", str(empty), "--out", str(self.tmp / "n.md"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected exactly one nevr-runtime-v<X.Y.Z>-windows.zip, found 0", result.stderr)
        hollow = self.tmp / "hollow"
        hollow.mkdir()
        with zipfile.ZipFile(hollow / "nevr-runtime-v5.0.0-windows.zip", "w") as z:
            z.writestr("README.txt", "x")
        result = self.run_notes("--assets", str(hollow), "--out", str(self.tmp / "n.md"))
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
                              "install-nevr-runtime.exe", "install.ps1", "nevr-install.exe", "nevr-runtime.dll", "quest-diagnose.ps1", "uninstall.ps1"])
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

    def test_the_tree_stage_refuses_a_missing_or_non_pe_installer_exe(self):
        """ADR 0023: the core installer rides in the zip, so the manifest's `contents` covers it."""
        self.installer.write_bytes(b"not a PE")
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: install-nevr-runtime.exe is missing or not a PE file", result.stderr)
        self.assertFalse(self.out.exists())
        self.installer.unlink()
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("install-nevr-runtime.exe is missing or not a PE file", result.stderr)

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

    def test_seal_refuses_a_tree_without_the_installer_exe(self):
        tree = self.make_tree()
        (tree / "install-nevr-runtime.exe").unlink()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("has no install-nevr-runtime.exe", result.stderr)
        self.assertFalse((self.tmp / "s").exists())

    def test_seal_refuses_a_tree_without_the_old_name_copy(self):
        """#1492: nevr-install.exe stays in the zip for programs that still call it, until the removal PR deletes it."""
        tree = self.make_tree()
        (tree / "nevr-install.exe").unlink()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("has no nevr-install.exe", result.stderr)
        self.assertFalse((self.tmp / "s").exists())

    def test_the_tree_holds_the_old_name_as_the_same_bytes_as_the_new_name(self):
        tree = self.make_tree()
        self.assertEqual((tree / "nevr-install.exe").read_bytes(), (tree / "install-nevr-runtime.exe").read_bytes())
        self.assertEqual((tree / "install-nevr-runtime.exe").read_bytes(), self.installer.read_bytes())

    def test_the_tree_stage_refuses_an_old_name_copy_with_other_bytes(self):
        """#1492: one build, copied once. Two binaries under the two names would be two builds."""
        self.installer_compat.write_bytes(b"MZ\0another build of the installer\0")
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: nevr-install.exe is not the same bytes as install-nevr-runtime.exe", result.stderr)
        self.assertFalse(self.out.exists())

    def test_the_tree_stage_refuses_a_missing_or_non_pe_old_name_copy(self):
        self.installer_compat.write_bytes(b"not a PE")
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: nevr-install.exe is missing or not a PE file", result.stderr)
        self.assertFalse(self.out.exists())
        self.installer_compat.unlink()
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("nevr-install.exe is missing or not a PE file", result.stderr)

    def test_the_readme_names_the_new_installer_and_says_the_old_name_is_the_same_file(self):
        tree = self.make_tree()
        readme = (tree / "README.txt").read_text()
        self.assertIn("install-nevr-runtime.exe", readme)
        self.assertEqual(readme.count("nevr-install.exe"), 1, "one sentence, for programs that still call the old name")
        sentence = next(line for line in readme.splitlines() if "nevr-install.exe" in line)
        self.assertRegex(sentence, r"same file")

    def test_the_signing_text_lists_both_installer_names_as_signed(self):
        tree = self.make_tree()
        sealed = self.seal_to(tree)
        with zipfile.ZipFile(sealed / "nevr-runtime-v5.0.0-windows.zip") as z:
            signing = z.read("SIGNING.txt").decode()
        self.assertIn("install-nevr-runtime.exe", signing)
        self.assertIn("nevr-install.exe", signing)

    def test_seal_of_a_normal_tree_lists_exactly_the_windows_zip_members(self):
        sealed = self.seal_to(self.make_tree())
        with zipfile.ZipFile(sealed / "nevr-runtime-v5.0.0-windows.zip") as z:
            names = set(z.namelist())
        self.assertEqual(names, {"BugSplat64.dll", "nevr-runtime.dll", "echovr_server.exe", "install-nevr-runtime.exe",
                                  "nevr-install.exe",
                                 "install.ps1", "uninstall.ps1", "quest-diagnose.ps1", "README.txt",
                                 "SHA256SUMS", "SIGNING.txt"})

    def test_seal_refuses_a_directory_in_the_tree_and_writes_nothing(self):
        tree = self.make_tree()
        (tree / "content" / "echocombat").mkdir(parents=True)
        (tree / "content" / "echocombat" / "pack.bin").write_bytes(b"game data")
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("content is not a regular file", result.stderr)
        self.assertFalse((self.tmp / "s").exists())

    def test_seal_refuses_a_stray_file_that_is_not_a_zip_member_and_names_it(self):
        tree = self.make_tree()
        (tree / "echocombat.zip").write_bytes(b"PK\x05\x06" + b"\0" * 18)
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("echocombat.zip is not a Windows zip member", result.stderr)
        self.assertIn("allowed:", result.stderr)
        self.assertIn("install.ps1", result.stderr)
        self.assertFalse((self.tmp / "s").exists())

    def test_seal_refuses_a_link_even_when_it_has_a_zip_member_name(self):
        tree = self.make_tree()
        target = self.tmp / "elsewhere.txt"
        target.write_text("a file outside the tree", encoding="utf-8")
        (tree / "README.txt").unlink()
        (tree / "README.txt").symlink_to(target)
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("README.txt is not a regular file", result.stderr)
        self.assertFalse((self.tmp / "s").exists())

    def test_seal_refuses_a_stray_plugins_directory_and_names_it(self):
        tree = self.make_tree()
        (tree / "plugins").mkdir()
        (tree / "plugins" / "broadcaster.dll").write_bytes(b"MZ")
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("plugins is not a regular file", result.stderr)
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
                       "Wi-Fi name", "numeric user ids", "Do not attach it to a public GitHub issue",
                       "never in a public channel"):
            self.assertIn(needle, texts["README.txt"], needle)
        # No sentence may be false in a package built anywhere: not "built by CI", not "UNSIGNED".
        self.assertNotIn("built by CI", package_release.SIGNING_TEXT)
        self.assertNotIn("No Quest APK", texts["RELEASE-NOTES.md"])
        self.assertNotIn("adb install", texts["RELEASE-NOTES.md"])
        self.assertNotIn("windows-server", texts["RELEASE-NOTES.md"])
        self.assertNotRegex(texts["RELEASE-NOTES.md"], r"-(lite|debug)\.zip")

    def test_the_notes_signing_section_states_the_default_policy_and_the_flag_that_enforces_it(self):
        # Since #933 a run loads unsigned plugins unless -require-signed-plugins is given. The notes must say that,
        # and the source must still say it: kDefaultMode (plugin_trust.cpp) and the flag's registration (cli.cpp).
        # The release body is short and says none of it; the tester notes of a local package (rendered from this
        # template, LocalDevPackageTest) do.
        notes = (PACKAGE / "RELEASE-NOTES.template.md").read_text(encoding="utf-8")
        self.assertIn("loads unsigned plugins by default", notes)
        self.assertIn("`-require-signed-plugins`", notes)
        self.assertNotIn("loads only plugins signed", notes)
        plugin_trust = (REPO / "src/runtime/ext/plugin_trust.cpp").read_text()
        self.assertIn("constexpr Mode kDefaultMode = Mode::UnsignedAllowed;", plugin_trust)
        cli = (REPO / "src/runtime/lifecycle/cli.cpp").read_text()
        self.assertIn('AddArgSyntax(pArgSyntax, "-require-signed-plugins"', cli)

    def test_the_zip_readme_tells_an_operator_how_to_start_a_server_and_its_config_uses_keys_the_runtime_reads(self):
        sealed = self.seal_to(self.make_tree())
        with zipfile.ZipFile(sealed / "nevr-runtime-v5.0.0-windows.zip") as z:
            readme = z.read("README.txt").decode()
        section = readme.split("Running a dedicated server\n", 1)[1].split("\nUpdate\n", 1)[0]
        prose = " ".join(section.split())  # the README wraps lines; a phrase may break across one
        for needle in (r".\echovr_server.exe", "-server -headless -no-console", r"%LOCALAPPDATA%\nevr\echovr\config.yaml",
                       "services.socket_uri", "issue #843", "signs in by device code", "no username or password",
                       "requested game server registration", "Received registration success",
                       "Graceful shutdown initiated",
                       "services.allow_offline_server"):
            self.assertIn(needle, prose, needle)
        # A server signs in by device code: no instruction to set a password, and no config block to copy.
        self.assertNotIn("auth.password", prose)
        self.assertNotRegex(prose, r"(?i)nevr_password|discord_id")
        # #843 / #1035: the service addresses and key come from the build; the keys the README names for another
        # service are keys the runtime maps.
        paths = set(re.findall(r"\b((?:services|auth)\.[a-z_]+)\b", prose)) - {"auth.server_key"}
        mapped = set(re.findall(r'\{"[a-z_]+", "([a-z_]+\.[a-z_]+)"\}',
                                (REPO / "src/runtime/lifecycle/service_map.cpp").read_text(encoding="utf-8")))
        self.assertEqual(paths - mapped, set(),
                         "the README names a key service_map.cpp does not map")
        self.assertNotRegex(prose, r"(wss?|https?)://\S*[?&@]", "no credential rides in a service address")

    def test_the_repository_readme_that_ships_in_the_dist_zips_makes_no_signing_fact(self):
        # README.md is copied into nevr-runtime-vX.Y.Z.zip, which is attested and never
        # change: it must be true whether or not the Windows zip has been signed.
        readme = (REPO / "README.md").read_text(encoding="utf-8")
        self.assertNotRegex(readme, r"(?i)\bunsigned\b|is not signed")
        self.assertEqual(words_of_the_old_model(readme), [])

    def test_the_readme_tells_a_hand_install_to_restore_the_game_file_before_install_ps1(self):
        # #957: install.ps1 cannot record the game's own BugSplat64.dll that a hand install replaced, so the README's
        # restore step comes before the line that runs install.ps1.
        readme = (REPO / "README.md").read_text(encoding="utf-8")
        section = readme.split("## Install for testers", 1)[1].split("### Moving a Quest", 1)[0]
        self.assertIn("installer\\aside\\BugSplat64.dll.original", section)
        self.assertIn("Verify Files", section)
        self.assertLess(section.index("Verify Files"), section.index("Then run `install.ps1`"))

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
        signed_installer = fake_pe(0x100, (tree / "install-nevr-runtime.exe").read_bytes())
        (tree / "install-nevr-runtime.exe").write_bytes(signed_installer)
        # The signer signs each entry on its own and stamps each with its own time, so after signing the two entries
        # need not be the same bytes: only the tree stage (before signing) demands that.
        signed_compat = fake_pe(0x100, b"another timestamp " + (tree / "nevr-install.exe").read_bytes())
        (tree / "nevr-install.exe").write_bytes(signed_compat)
        self.sign_scripts(tree)
        sealed = self.seal_to(tree, "after", "--require-signed")
        zip_path = sealed / "nevr-runtime-v5.0.0-windows.zip"
        with zipfile.ZipFile(zip_path) as z, zipfile.ZipFile(before / "nevr-runtime-v5.0.0-windows.zip") as zb:
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            self.assertEqual(sums["BugSplat64.dll"], hashlib.sha256(signed).hexdigest())
            self.assertEqual(sums["nevr-runtime.dll"], hashlib.sha256(signed_runtime).hexdigest())
            self.assertEqual(sums["echovr_server.exe"], hashlib.sha256(signed_server).hexdigest())
            self.assertEqual(sums["install-nevr-runtime.exe"], hashlib.sha256(signed_installer).hexdigest())
            self.assertEqual(sums["nevr-install.exe"], hashlib.sha256(signed_compat).hexdigest())
            self.assertNotEqual(sums["BugSplat64.dll"], unsigned_sha)
            self.assertEqual(z.read("SIGNING.txt"), zb.read("SIGNING.txt"), "the signing text never changes")
            self.assertEqual(z.read("README.txt"), zb.read("README.txt"))
            self.assertEqual(z.namelist(), zb.namelist(), "same entry names and order")
            for name in z.namelist():
                if name != "SHA256SUMS":
                    self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)
        self.assertEqual(self.notes_for(sealed, "n-after").read_text(), self.notes_for(before, "n-before").read_text(),
                         "the body is the same before and after signing: it holds no hash")

    def test_require_signed_is_a_gate_only_and_changes_no_text(self):
        tree = self.make_tree()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"), "--require-signed")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not signed (no certificate table): BugSplat64.dll, echovr_server.exe, install-nevr-runtime.exe, nevr-install.exe, nevr-runtime.dll", result.stderr)
        self.assertFalse((self.tmp / "s").exists())
        (tree / "BugSplat64.dll").write_bytes(fake_pe(0x100, b"x"))
        (tree / "nevr-runtime.dll").write_bytes(fake_pe(0x100, b"z"))
        (tree / "echovr_server.exe").write_bytes(fake_pe(0x100, b"y"))
        (tree / "install-nevr-runtime.exe").write_bytes(fake_pe(0x100, b"i"))
        (tree / "nevr-install.exe").write_bytes(fake_pe(0x100, b"j"))
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

    def test_quest_library_check_accepts_the_shim_as_the_build_writes_it_without_the_full_commit(self):
        """The shim carries kVersion and the short kCommit and no full commit: the build record that held it is compiled
        into the runtime library (ThisBuild, src/quest/integration/production_steps.cpp), which quest-runtime checks."""
        library = self.quest_library(b"token_auth_state", RELEASE.encode(), SHORT_COMMIT.encode(), SHIM_BUILD_ID.encode())
        result = self.quest_check(library)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"quest library: version {RELEASE} commit {COMMIT[:8]}", result.stdout)

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
                        (version, full)):  # the full commit alone is not kCommit
            result = self.quest_check(self.quest_library(*strings))
            self.assertEqual(result.returncode, 1, strings)
            self.assertIn("no string of 7 to 39 hex digits that is a prefix of the commit", result.stderr)

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

    def test_a_dev_host_is_never_stamped_or_packaged(self):
        # #1122: the host has no plugin-trust literal, yet a host built as the plugin developers' build loads an
        # unsigned nevr-runtime.dll, so it carries a literal of its own and the release path reads it.
        self.dll.write_bytes(host_binary(RELEASE, build=(b"NEVR-HOST-BUILD dev",)))
        result = self.stamp("--version", RELEASE, dll=self.dll)
        self.assertEqual(result.returncode, 1)
        self.assertIn("NEVR-HOST-BUILD is 'dev', not 'release'", result.stderr)
        self.dll.write_bytes(host_binary(DEV, build=(b"NEVR-HOST-BUILD dev",)))
        self.assertEqual(self.stamp(dll=self.dll).returncode, 1)  # refused in a development package too: it can be handed to players

    def test_a_host_without_the_host_build_literal_is_refused_and_so_are_two(self):
        self.dll.write_bytes(host_binary(RELEASE, build=()))
        self.assertIn("found 0", self.stamp("--version", RELEASE, dll=self.dll).stderr)
        self.dll.write_bytes(host_binary(RELEASE, build=(b"NEVR-HOST-BUILD release",) * 2))
        self.assertIn("found 2", self.stamp("--version", RELEASE, dll=self.dll).stderr)

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
