"""`just android-repack-apk` end to end: the versionCode of the APK it BUILDS (#923).

Android refuses an update whose versionCode is lower than the installed one (INSTALL_FAILED_VERSION_DOWNGRADE).
The community Quest installer's game is versionCode 4987570; the store APK the recipe repacks is 4987566. The
recipe therefore sets the versionCode at repack time, from the build's own version, above 4987570 (the numbering
rule is in tools/quest_apk_version.py).

Nothing here reads the recipe's text. The recipe runs for real, over a fixture APK and a compiled fixture shim,
signed through the real tools/quest_keystore.sh with a fixture key made here, named by NEVR_QUEST_KEYSTORE and
NEVR_QUEST_KEYSTORE_PASS (the real key is never touched), and the versionCode is read back from the SIGNED APK by aapt2, a reader that is not ours.
The expected numbers are written out here, derived by hand from the rule, not computed by the tool.
"""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "tests"))
import work_root  # noqa: E402
import axml_fixtures as fx  # noqa: E402
import quest_apk_version as qv  # noqa: E402
from quest_patcher_standin import MARKER, make_standin  # noqa: E402
from test_quest_keystore import ALIAS, PASSPHRASE  # noqa: E402

JUSTFILE = REPO / "justfile"
INSTALLER_VERSION_CODE = 4987570  # the game build the community installer pins
STORE_VERSION_CODE = 4987566  # the store APK the recipe repacks
TOOLS = ("just", "cc", "patchelf", "zip", "unzip", "zipalign", "apksigner", "aapt2", "readelf", "nm", "keytool")
MISSING = [t for t in TOOLS if shutil.which(t) is None]

# The recipe stamps only at or above the newest release tag's major (the stamp floor, tools/quest_apk_version.py), so the
# fixture versions take that major from git rather than pin it: a new major must not stale this file (#1371).
MAJOR = qv.newest_release()[1]
DEV_VERSION = f"{MAJOR}.6.1-dev.37+3a35e0b"
DEV_CODE = MAJOR * 10**7 + 6 * 10**5 + 1 * 10**3 + 37
RELEASE_VERSION = f"{MAJOR}.6.0"
RELEASE_CODE = MAJOR * 10**7 + 6 * 10**5 + 0 * 10**3 + 999

SHIM_C = r'''
/* The shim's stand-in: exports the marker the recipe greps for and carries its build version NUL-bounded. */
int nevr_sentinel_marker = 1;
const char nevr_fixture_version[] = "\0" VERSION "\0";
'''
ORIGINAL_C = "int nevr_original_loader_symbol = 2;\n"


def build_lib(path: pathlib.Path, source: str, soname: str, version: str = "") -> None:
    src = path.with_suffix(".c")
    src.write_text(source)
    cmd = ["cc", "-shared", "-fPIC", f"-Wl,-soname,{soname}", "-o", str(path), str(src)]
    if version:
        cmd.insert(1, f'-DVERSION="{version}"')
    subprocess.run(cmd, check=True, capture_output=True)


def badging_code(apk: pathlib.Path) -> int:
    out = subprocess.run(["aapt2", "dump", "badging", str(apk)], capture_output=True, text=True, check=True).stdout
    return int(re.search(r"^package: .*versionCode='(\d+)'", out, re.M).group(1))


def xmltree(apk: pathlib.Path) -> str:
    return subprocess.run(["aapt2", "dump", "xmltree", "--file", "AndroidManifest.xml", str(apk)],
                          capture_output=True, text=True, check=True).stdout


@unittest.skipIf(MISSING, f"the recipe needs {', '.join(MISSING)}")
class RepackFixture(unittest.TestCase):
    """One scratch directory: fixture key, store-like APK, compiled shim, build info."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-version-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        run = self.tmp / "run"
        run.mkdir(mode=0o700)
        ks = self.tmp / "fixture.jks"
        subprocess.run(["keytool", "-genkeypair", "-keystore", str(ks), "-alias", ALIAS, "-keyalg", "RSA",
                        "-keysize", "2048", "-validity", "30", "-storepass", PASSPHRASE, "-keypass", PASSPHRASE,
                        "-dname", "CN=Quest Repack Version Test, O=test"], capture_output=True, check=True)
        listing = subprocess.run(["keytool", "-list", "-keystore", str(ks), "-alias", ALIAS, "-storepass", PASSPHRASE],
                                 capture_output=True, text=True, check=True).stdout
        line = next(l for l in listing.splitlines() if "SHA-256" in l or "SHA256" in l)
        self.signer = line.split(":", 1)[1].strip().replace(":", "").lower()
        self.env = dict(os.environ, NEVR_QUEST_KEYSTORE=str(ks), NEVR_QUEST_KEYSTORE_PASS=PASSPHRASE,
                        XDG_RUNTIME_DIR=str(run))
        # The real libr15 patcher refuses any file but the store's, so the recipe is given a stand-in that appends a marker
        # (docs/adr/0021 section 9: quest_patcher_standin.py); the recipe's `patcher` parameter is for this.
        self.patcher = make_standin(self.tmp / "patcher")

    def make_inputs(self, version: str, *, manifest: bytes = None, header: bool = True, shim_version: str = None):
        """A store-like APK (versionCode 4987566, as the store's), the shim and its build info."""
        original = self.tmp / "original" / "libovrplatformloader.so"
        original.parent.mkdir(exist_ok=True)
        build_lib(original, ORIGINAL_C, "libovrplatformloader.so")
        self.original_bytes = original.read_bytes()
        self.apk = self.tmp / "store.apk"
        with zipfile.ZipFile(self.apk, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("AndroidManifest.xml", manifest if manifest is not None else fx.build_manifest(STORE_VERSION_CODE))
            z.writestr("lib/arm64-v8a/libovrplatformloader.so", self.original_bytes)
            z.writestr("lib/arm64-v8a/libr15.so", b"\x7fELF the game\0" * 50)
            z.writestr("assets/data.bin", bytes(range(256)) * 20)
            z.writestr("META-INF/CERT.SF", b"signature file\n")
            z.writestr("META-INF/CERT.RSA", b"\x30\x00")
            z.writestr("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\n")
        self.shim = self.tmp / "build" / "sentinel" / "libovrplatformloader.so"
        self.shim.parent.mkdir(parents=True)
        build_lib(self.shim, SHIM_C, "libovrplatformloader.so", shim_version if shim_version is not None else version)
        if header:
            info = self.tmp / "build" / "generated"
            info.mkdir()
            (info / "nevr_build_info.h").write_text(
                f'inline constexpr const char* kVersion = "{version}";\n'
                'inline constexpr const char* kCommit = "3a35e0b9";\n')

    def repack(self):
        work = self.tmp / "work"
        result = subprocess.run(
            ["just", "--justfile", str(JUSTFILE), "--working-directory", str(REPO), "android-repack-apk",
             str(self.apk), str(self.shim), os.path.relpath(work, REPO), "", "", "", str(self.patcher)],
            capture_output=True, text=True, env=self.env, timeout=300)
        self.signed = work / "r15_nevr-sentinel_signed.apk"
        return result


class RepackedVersionCodeTest(RepackFixture):
    """The versionCode of the APK the recipe builds, read back from the signed file."""

    def test_the_built_apk_is_above_the_installers_game_build(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertGreater(badging_code(self.signed), INSTALLER_VERSION_CODE,
                           "an update over a headset on the installer's build would be a downgrade")

    def test_a_development_build_gets_the_code_the_rule_gives_it(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(badging_code(self.signed), DEV_CODE)

    def test_a_release_gets_a_code_above_every_development_build_of_it_and_below_the_next(self):
        self.make_inputs(RELEASE_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        code = badging_code(self.signed)
        self.assertEqual(code, RELEASE_CODE)
        self.assertGreater(code, RELEASE_CODE - 1)  # above the same major's dev.998, the slot just below the release's 999
        self.assertLess(code, DEV_CODE)  # below the next patch's development builds

    def test_the_signed_apk_verifies_and_carries_the_signer_of_the_key_in_pass(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        verify = subprocess.run(["apksigner", "verify", "--verbose", "--print-certs", str(self.signed)],
                                capture_output=True, text=True)
        self.assertEqual(verify.returncode, 0, verify.stdout + verify.stderr)
        # apksigner picks the schemes from the manifest's minSdkVersion: 29 (the store APK's, and the fixture's) gets v3
        # alone. The same is true of the APK the recipe built before the versionCode was set.
        self.assertIn("Verified using v3 scheme (APK Signature Scheme v3): true", verify.stdout)
        self.assertIn(f"certificate SHA-256 digest: {self.signer}", verify.stdout)

    def test_only_the_versionCode_line_of_the_manifest_changes(self):
        self.make_inputs(DEV_VERSION)
        before = xmltree(self.apk).splitlines()
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        after = xmltree(self.signed).splitlines()
        changed = [(b, a) for b, a in zip(before, after) if b != a]
        self.assertEqual(len(before), len(after))
        self.assertEqual(len(changed), 1, changed)
        self.assertIn(f"versionCode(0x0101021b)={STORE_VERSION_CODE}", changed[0][0])
        self.assertIn(f"versionCode(0x0101021b)={DEV_CODE}", changed[0][1])

    def test_every_other_entry_of_the_store_apk_is_byte_identical(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.apk) as src, zipfile.ZipFile(self.signed) as out:
            kept = [n for n in src.namelist()
                    if not n.startswith("META-INF/") and n not in ("AndroidManifest.xml", "lib/arm64-v8a/libovrplatformloader.so")]
            self.assertEqual(sorted(kept), ["assets/data.bin", "lib/arm64-v8a/libr15.so"])
            for name in kept:
                # libr15.so is the one entry the repack changes on purpose: it is the patcher's output (here the stand-in's)
                expected = src.read(name) + MARKER if name == "lib/arm64-v8a/libr15.so" else src.read(name)
                self.assertEqual(out.read(name), expected, name)
            self.assertIn("lib/arm64-v8a/libovrplatformloader_orig.so", out.namelist())

    def test_the_recipe_says_what_it_stamped(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f'"version_code_after":{DEV_CODE}', result.stderr.replace(" ", ""))
        self.assertIn(f"versionCode {DEV_CODE}", result.stdout)


class RepackRefusalTest(RepackFixture):
    """The recipe stops, with nothing signed, when it cannot be sure the versionCode it ships is the right one."""

    def assert_refused(self, result, *needles):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.signed.exists(), "a signed APK was left behind")
        text = result.stdout + result.stderr
        for needle in needles:
            self.assertIn(needle, text)

    def test_no_build_info_beside_the_shim_is_a_refusal_not_a_default(self):
        self.make_inputs(DEV_VERSION, header=False)
        self.assert_refused(self.repack(), "nevr_build_info.h")

    def test_a_shim_that_does_not_carry_the_build_version_is_refused(self):
        self.make_inputs(DEV_VERSION, shim_version=f"{MAJOR}.6.1-dev.36+3a35e0b")
        self.assert_refused(self.repack(), "shim", DEV_VERSION)

    def test_a_manifest_without_a_versionCode_is_refused(self):
        self.make_inputs(DEV_VERSION, manifest=fx.build_manifest(version_code_count=0))
        self.assert_refused(self.repack(), "versionCode")

    def test_a_version_below_the_last_release_is_refused(self):
        self.make_inputs("0.1.0")
        self.assert_refused(self.repack(), f"major {MAJOR}")

    def test_the_version_a_build_without_git_reports_is_refused(self):
        # cmake/set_project_version_from_git.cmake leaves "1.0.0" when git is absent: a release-looking version
        # that would give a code far below every published build's
        self.make_inputs("1.0.0")
        self.assert_refused(self.repack(), "1.0.0", f"major {MAJOR}")

    def fake_tool(self, name, body):
        """A `name` earlier on PATH than the real one; `body` is bash and may call $REAL (the real tool's path)."""
        real = shutil.which(name)
        fake = self.tmp / "fakebin" / name
        fake.parent.mkdir(exist_ok=True)
        fake.write_text(f"#!/usr/bin/env bash\nREAL={real}\n{body}\n")
        fake.chmod(0o755)
        self.env["PATH"] = f"{fake.parent}:{self.env['PATH']}"

    def test_a_signed_apk_that_reads_back_with_another_code_is_removed_and_the_recipe_fails(self):
        # The recipe compares what aapt2 reads from the SIGNED APK with what it stamped. Put an aapt2 on PATH
        # that reports a different code, and the recipe must refuse and leave no signed APK for package-dev.
        self.make_inputs(DEV_VERSION)
        self.fake_tool("aapt2", "\"$REAL\" \"$@\" | sed \"s/versionCode='[0-9]*'/versionCode='1'/\"")
        self.assert_refused(self.repack(), "signed APK versionCode is '1'", str(DEV_CODE))

    def test_an_aapt2_that_fails_leaves_no_signed_apk(self):
        # set -e quits at a failing command substitution: the APK must not survive for quest-install and the pinned tests
        self.make_inputs(DEV_VERSION)
        self.fake_tool("aapt2", "echo 'aapt2: cannot read the apk' >&2; exit 3")
        self.assert_refused(self.repack(), "aapt2: cannot read the apk")

    def test_an_apksigner_verify_that_fails_leaves_no_signed_apk(self):
        # `sign` is real (so a signed APK exists to be left behind); only `verify` fails
        self.make_inputs(DEV_VERSION)
        self.fake_tool("apksigner", "if [ \"$1\" = verify ]; then echo 'apksigner: DOES NOT VERIFY' >&2; exit 1; fi\n\"$REAL\" \"$@\"")
        self.assert_refused(self.repack(), "apksigner: DOES NOT VERIFY")

    def test_a_failure_after_the_checks_leaves_no_signed_apk_either(self):
        # the ELF checks that follow (here `nm`) run on the signed APK; any stop after signing removes it
        self.make_inputs(DEV_VERSION)
        self.fake_tool("nm", "echo 'nm: no symbols' >&2; exit 1")
        self.assert_refused(self.repack(), "nm: no symbols")

    def test_a_signed_apk_from_an_earlier_run_does_not_survive_a_refused_one(self):
        self.make_inputs(DEV_VERSION)
        first = self.repack()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertTrue(self.signed.exists())
        (self.tmp / "build" / "generated" / "nevr_build_info.h").unlink()
        self.assert_refused(self.repack(), "nevr_build_info.h")


if __name__ == "__main__":
    unittest.main()
