"""The fixture of `just android-repack-apk` for the Python repack tests (#923).

The cases that stood here (the versionCode of the APK the recipe builds, and the recipe's refusals) are ported to Go:
cmd/quest-apk-version/repack_test.go, which runs the same recipe over the same fixture. What stays is RepackFixture, the
fixture test_quest_repack_libs.py and test_quest_repack_pack.py import; it ends with those tests' port.

The original docstring of the cases follows.

`just android-repack-apk` end to end: the versionCode of the APK it BUILDS (#923).

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
