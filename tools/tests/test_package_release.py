"""package_release.py: the gate and the assembly, with fake artifacts; install.ps1/uninstall.ps1 under pwsh.

A release is a version plus GitHub's pre-release flag. The packaging path has NO flag input and no word of
the zip, SHA256SUMS, RELEASE-NOTES.md or SIGNING.txt depends on it: promoting a release changes no byte.
A local build is a development build (X.Y.(Z+1)-dev.N+sha) and is packaged under a dev name.
"""

import hashlib
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

COMMIT = "3a35e0b945456dba8a9b56e8110df330178e7259"
DEV = "5.0.1-dev.3+3a35e0b"
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


def binary(version, commit=COMMIT, literals=1, extra=b"", trust=(b"NEVR-PLUGIN-TRUST signed-only",)):
    """A fake binary: the defaults, the identity literal NUL-bounded `literals` times, the plugin-trust
    literal(s) in `trust` (ADR 0010), then `extra`."""
    parts = [v.encode() for v in VALUES.values()] + [identity(version, commit)] * literals + list(trust)
    return b"MZ" + b"\0" + b"\0".join(parts) + b"\0" + extra


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
        self.dll.write_bytes(binary(self.version))
        self.apk = self.tmp / "signed.apk"
        self.write_apk(self.version)
        self.out = self.tmp / "out"

    def write_apk(self, version):
        sentinel = b"\x7fELF" + b"\0".join([v.encode() for v in VALUES.values()] + [version.encode(), COMMIT[:8].encode()])
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", sentinel)
            z.writestr("AndroidManifest.xml", b"<m/>")

    def run_tool(self, **overrides):
        """The local path (`just package-dev`): no --version, the binaries must carry a dev version."""
        args = {"--commit": COMMIT, "--out": str(self.out), "--dll": str(self.dll),
                "--apk": str(self.apk), "--pc-header": str(self.pc_header),
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
                             ["BugSplat64.dll", "README.txt", "SHA256SUMS", "SIGNING.txt", "install.ps1",
                              "quest-diagnose.ps1", "uninstall.ps1"])
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
        self.dll.write_bytes(binary(RELEASE))
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
        self.dll.write_bytes(binary(self.version, literals=2))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("found 2", result.stderr)

    def test_a_literal_that_is_not_nul_bounded_is_not_the_identity(self):
        self.dll.write_bytes(b"MZ" + b"x" + identity(self.version) + b"y")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("found 0", result.stderr)

    def test_the_wrong_commit_is_refused(self):
        self.dll.write_bytes(binary(self.version, commit="f" * 40))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the checked-out commit", result.stderr)

    def test_a_local_run_has_no_version_option(self):
        result = self.run_tool(**{"--version": "5.0.0"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments: --version", result.stderr)
        self.assertFalse(self.out.exists())

    def test_a_dll_that_embeds_no_endpoints_is_refused_and_nothing_is_written(self):
        self.dll.write_bytes(b"MZ\0" + identity(self.version) + b"\0")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: NEVR_SOCKET_URI: not embedded in BugSplat64.dll", result.stderr)
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
                "--pc-header": str(self.pc_header), "--defaults": str(self.defaults)}
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
                         ["BugSplat64.dll", "README.txt", "install.ps1", "quest-diagnose.ps1", "uninstall.ps1"])
        info = (self.out / "release.json").read_text()
        self.assertIn('"version": "5.0.0"', info)
        self.assertIn(COMMIT, info)

    def test_the_tree_stage_refuses_a_development_version(self):
        self.dll.write_bytes(binary(DEV))
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the expected '5.0.0'", result.stderr)
        self.assertFalse(self.out.exists())

    def test_the_tree_stage_refuses_another_release_version_or_commit(self):
        self.dll.write_bytes(binary("5.0.1"))
        self.assertEqual(self.run_stage("tree", *self.tree_args()).returncode, 1)
        self.dll.write_bytes(binary(RELEASE, commit="e" * 40))
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the checked-out commit", result.stderr)

    def test_the_tree_stage_refuses_a_version_that_is_not_x_y_z(self):
        for bad in ("v5.0.0", "5.0", "5.0.0-rc.1", "5.0.0-dev.1+abc1234", "05.0.0", "5.00.0", "5.0.00", "5.0.0+x"):
            with self.subTest(version=bad):
                result = self.run_stage("tree", *self.tree_args(**{"--version": bad}))
                self.assertEqual(result.returncode, 1)
                self.assertIn("is not X.Y.Z", result.stderr)

    def test_the_tree_stage_refuses_a_dll_that_embeds_no_defaults(self):
        self.dll.write_bytes(b"MZ\0" + identity(RELEASE) + b"\0")
        result = self.run_stage("tree", *self.tree_args())
        self.assertEqual(result.returncode, 1)
        self.assertIn("windows: NEVR_SOCKET_URI: not embedded in BugSplat64.dll", result.stderr)

    def test_the_file_names_follow_the_tag_version(self):
        self.dll.write_bytes(binary("5.1.2"))
        result = self.run_stage("tree", *self.tree_args(**{"--version": "5.1.2"}))
        self.assertEqual(result.returncode, 0, result.stderr)
        tree = self.out / "nevr-runtime-v5.1.2-windows"
        sealed = self.seal_to(tree)
        self.assertEqual(sorted(p.name for p in sealed.iterdir()),
                         ["RELEASE-NOTES.md", "SHA256SUMS", "nevr-runtime-v5.1.2-windows.zip"])

    def sign_scripts(self, tree):
        """What signing does to a PowerShell script: an Authenticode block is appended."""
        for name in ("install.ps1", "uninstall.ps1", "quest-diagnose.ps1"):
            path = tree / name
            path.write_bytes(path.read_bytes() + b"\n# SIG # Begin signature block\n# SIG # End signature block\n")

    def test_a_release_is_the_zip_sums_and_notes_with_no_quest_apk(self):
        tree = self.make_tree()
        sealed = self.seal_to(tree)
        self.assertEqual(sorted(p.name for p in sealed.iterdir()),
                         ["RELEASE-NOTES.md", "SHA256SUMS", "nevr-runtime-v5.0.0-windows.zip"])
        notes = (sealed / "RELEASE-NOTES.md").read_text()
        self.assertIn("No Quest APK in this release", notes)
        self.assertNotIn("adb install", notes)
        self.assertNotIn("Two artifacts", notes)
        top = (sealed / "SHA256SUMS").read_text()
        zip_path = sealed / "nevr-runtime-v5.0.0-windows.zip"
        self.assertEqual(top, f"{hashlib.sha256(zip_path.read_bytes()).hexdigest()}  {zip_path.name}\n")

    def test_the_texts_state_facts_that_hold_before_and_after_signing_and_use_no_old_model_words(self):
        tree = self.make_tree()
        sealed = self.seal_to(tree)
        with zipfile.ZipFile(sealed / "nevr-runtime-v5.0.0-windows.zip") as z:
            texts = {"SIGNING.txt": z.read("SIGNING.txt").decode(), "README.txt": z.read("README.txt").decode()}
        texts["RELEASE-NOTES.md"] = (sealed / "RELEASE-NOTES.md").read_text()
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
        self.assertIn("Two zips", texts["RELEASE-NOTES.md"])
        self.assertIn("nevr-runtime-v5.0.0.zip", texts["RELEASE-NOTES.md"])
        self.assertNotRegex(texts["RELEASE-NOTES.md"], r"-(lite|debug)\.zip")

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
        self.sign_scripts(tree)
        sealed = self.seal_to(tree, "after", "--require-signed")
        zip_path = sealed / "nevr-runtime-v5.0.0-windows.zip"
        with zipfile.ZipFile(zip_path) as z, zipfile.ZipFile(before / "nevr-runtime-v5.0.0-windows.zip") as zb:
            sums = dict(line.split("  ")[::-1] for line in z.read("SHA256SUMS").decode().splitlines())
            self.assertEqual(sums["BugSplat64.dll"], hashlib.sha256(signed).hexdigest())
            self.assertNotEqual(sums["BugSplat64.dll"], unsigned_sha)
            self.assertEqual(z.read("SIGNING.txt"), zb.read("SIGNING.txt"), "the signing text never changes")
            self.assertEqual(z.read("README.txt"), zb.read("README.txt"))
            self.assertEqual(z.namelist(), zb.namelist(), "same entry names and order")
            for name in z.namelist():
                if name != "SHA256SUMS":
                    self.assertEqual(sums[name], hashlib.sha256(z.read(name)).hexdigest(), name)
        self.assertEqual((sealed / "RELEASE-NOTES.md").read_bytes(), (before / "RELEASE-NOTES.md").read_bytes())
        self.assertIn(f"{hashlib.sha256(zip_path.read_bytes()).hexdigest()}  {zip_path.name}",
                      (sealed / "SHA256SUMS").read_text())

    def test_require_signed_is_a_gate_only_and_changes_no_text(self):
        tree = self.make_tree()
        result = self.run_stage("seal", "--tree", str(tree), "--out", str(self.tmp / "s"), "--require-signed")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not signed (no certificate table): BugSplat64.dll", result.stderr)
        self.assertFalse((self.tmp / "s").exists())
        (tree / "BugSplat64.dll").write_bytes(fake_pe(0x100, b"x"))
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
        for name in ("nevr-runtime-v5.0.0-windows.zip", "SHA256SUMS", "RELEASE-NOTES.md"):
            self.assertEqual((first / name).read_bytes(), (second / name).read_bytes(), name)

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
        return self.run_stage("stamp", "--dll", str(dll or self.dll), *args)

    def test_stamp_accepts_the_expected_release_version_and_commit(self):
        result = self.stamp("--version", RELEASE, "--commit", COMMIT)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"stamped identity: NEVR-BUILD 5.0.0 {COMMIT}", result.stdout)

    def test_stamp_refuses_a_dev_build_where_a_release_was_expected(self):
        self.dll.write_bytes(binary(DEV))
        result = self.stamp("--version", RELEASE)
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"version '{DEV}' is not the expected '5.0.0'", result.stderr)

    def test_stamp_without_a_version_wants_a_dev_build_and_refuses_a_release_stamp(self):
        self.assertEqual(self.stamp().returncode, 1)  # the fixture carries 5.0.0: a branch build must not
        self.dll.write_bytes(binary(DEV))
        self.assertEqual(self.stamp().returncode, 0)

    def test_stamp_refuses_a_missing_or_doubled_literal_and_the_wrong_commit(self):
        self.dll.write_bytes(b"MZ\0" + b"5.0.0\0")
        self.assertEqual(self.stamp("--version", RELEASE).returncode, 1)
        self.dll.write_bytes(binary(RELEASE, literals=2))
        self.assertIn("found 2", self.stamp("--version", RELEASE).stderr)
        self.dll.write_bytes(binary(RELEASE))
        result = self.stamp("--version", RELEASE, "--commit", "d" * 40)
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not the checked-out commit", result.stderr)

    # --- the plugin signature gate (ADR 0010, #741) --------------------------------------------------

    def test_a_binary_that_loads_unsigned_plugins_is_never_stamped_or_packaged(self):
        self.dll.write_bytes(binary(RELEASE, trust=(b"NEVR-PLUGIN-TRUST unsigned-allowed",)))
        result = self.stamp("--version", RELEASE)
        self.assertEqual(result.returncode, 1)
        self.assertIn("'unsigned-allowed', not 'signed-only'", result.stderr)
        self.dll.write_bytes(binary(DEV, trust=(b"NEVR-PLUGIN-TRUST unsigned-allowed",)))
        self.assertEqual(self.stamp().returncode, 1)  # a dev build is refused too: it can be handed to players

    def test_a_binary_without_the_plugin_trust_literal_is_refused_and_so_are_two(self):
        self.dll.write_bytes(binary(RELEASE, trust=()))
        self.assertIn("found 0", self.stamp("--version", RELEASE).stderr)
        self.dll.write_bytes(binary(RELEASE, trust=(b"NEVR-PLUGIN-TRUST signed-only",) * 2))
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

    # #759: an update must leave uninstall.ps1 able to put the oldest original back. A runtime DLL carries the
    # NEVR-BUILD identity literal, which install.ps1 uses to tell a runtime from the game's own file and
    # uninstall.ps1 uses to know a DLL is ours. Most tests use a plain payload (no literal) so the hash lists, not
    # the literal, are what they exercise.
    def set_package(self, payload):
        (self.pkg / "BugSplat64.dll").write_bytes(payload)
        (self.pkg / "SHA256SUMS").write_text(f"{hashlib.sha256(payload).hexdigest()}  BugSplat64.dll\n")

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

    def test_no_script_deletes_anything(self):
        for name in ("install.ps1", "uninstall.ps1"):
            text = (PACKAGE / name).read_text()
            self.assertNotIn("Remove-Item", text, name)
            self.assertNotRegex(text, r"\bdel\b|\brm\b", name)


if __name__ == "__main__":
    unittest.main()
