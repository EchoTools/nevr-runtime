"""tools/install_game_ndk.py and tools/game_ndk.pin: the game's NDK (r21b), pinned in this repository (#2047).

Offline: a small fixture zip laid out like the NDK, served over file://, stands in for the 1.1 GB archive. Each case
writes a pin of its own, runs the real script into a scratch root, and reads the exit code and the JSON log records. No
network and no real NDK. The pin file itself is checked against the values the store APK's libraries carry.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "install_game_ndk.py"
PIN = REPO / "tools" / "game_ndk.pin"

PINNED = (
    "Android (6317467 based on r365631c1) clang version 9.0.8 "
    "(https://android.googlesource.com/toolchain/llvm-project e0caee08e5f09b374a27a676d04978c81fcb1928) "
    "(based on LLVM 9.0.8svn)"
)
WRONG = (
    "Android (9352603, based on r450784d1) clang version 14.0.7 "
    "(https://android.googlesource.com/toolchain/llvm-project 4c603efb0cca074e9238af8b4106c30add4418f6)"
)
CLANG_REL = "toolchains/llvm/prebuilt/linux-x86_64/bin/clang"


def parse_pin(path: pathlib.Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            key, _, value = line.partition("=")
            values[key] = value
    return values


def make_fixture_zip(out: pathlib.Path, clang_first_line: str) -> None:
    """A zip laid out like the real archive (android-ndk-r21b/...), whose clang prints `clang_first_line`."""
    script = f"#!/usr/bin/env bash\necho '{clang_first_line}'\necho 'Target: x86_64-unknown-linux-gnu'\n"
    info = zipfile.ZipInfo(f"android-ndk-r21b/{CLANG_REL}")
    info.external_attr = 0o755 << 16
    with zipfile.ZipFile(out, "w") as zf:
        zf.writestr(info, script)
        zf.writestr("android-ndk-r21b/build/cmake/android.toolchain.cmake", "# fixture\n")


class InstallGameNdk(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="game-ndk-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.good_zip = self.tmp / "good.zip"
        make_fixture_zip(self.good_zip, PINNED)
        self.good_sha = hashlib.sha256(self.good_zip.read_bytes()).hexdigest()

    def pin_for(self, name: str, zip_path: pathlib.Path, sha: str, clang: str = PINNED, extra: str = "") -> pathlib.Path:
        pin = self.tmp / f"{name}.pin"
        pin.write_text(
            f"NDK_RELEASE=r21b\nNDK_BUILD=test\nNDK_URL={zip_path.as_uri()}\nNDK_SHA256={sha}\nCLANG_VERSION={clang}\n{extra}",
            encoding="utf-8",
        )
        return pin

    def run_script(self, root: pathlib.Path, pin: pathlib.Path, *extra: str) -> subprocess.CompletedProcess:
        env = {k: v for k, v in os.environ.items() if k not in ("XDG_CACHE_HOME",)}
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), "--pin", str(pin), "--root", str(root), *extra],
            capture_output=True, text=True, env=env, timeout=120,
        )

    @staticmethod
    def events(root: pathlib.Path) -> list[dict]:
        log = root / "ndk-install.log"
        return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()] if log.exists() else []

    def names(self, root: pathlib.Path) -> list[str]:
        return [e["event"] for e in self.events(root)]

    # --- the pin file ---

    def test_the_pin_file_carries_the_values_the_store_libraries_name(self):
        pin = parse_pin(PIN)
        self.assertEqual(pin, {
            "NDK_RELEASE": "r21b",
            "NDK_BUILD": "6352462",
            "NDK_URL": "https://dl.google.com/android/repository/android-ndk-r21b-linux-x86_64.zip",
            "NDK_SHA256": "0c7af5dd23c5d2564915194e71b1053578438ac992958904703161c7672cbed7",
            "CLANG_VERSION": PINNED,
        })

    # --- the install ---

    def test_a_good_archive_installs_logs_every_step_and_a_second_run_changes_nothing(self):
        root = self.tmp / "root1"
        pin = self.pin_for("p1", self.good_zip, self.good_sha)
        first = self.run_script(root, pin)
        self.assertEqual(first.returncode, 0, first.stderr)
        clang = root / "ndk-r21b" / CLANG_REL
        self.assertTrue(os.access(clang, os.X_OK), "the extracted clang keeps its execute bit")
        self.assertEqual(self.names(root), [
            "run_start", "download_start", "sha256_ok", "download_done", "extract_start", "clang_version_ok", "built"])
        for record in self.events(root):
            self.assertEqual(record["component"], "toolchain-ndk")
            self.assertIn("ts", record)
            self.assertIn("level", record)
        self.assertEqual([p.name for p in root.iterdir() if p.name.startswith(".build-")], [])
        before = len(self.events(root))
        second = self.run_script(root, pin)
        self.assertEqual(second.returncode, 0, second.stderr)
        later = [e["event"] for e in self.events(root)[before:]]
        self.assertIn("noop_reverified", later)
        self.assertNotIn("download_start", later)
        self.assertNotIn("extract_start", later)

    def test_a_wrong_sha256_is_refused_naming_both_hashes_and_leaves_nothing(self):
        root = self.tmp / "root2"
        bad = "0" * 64
        result = self.run_script(root, self.pin_for("p2", self.good_zip, bad))
        self.assertEqual(result.returncode, 1)
        mismatch = [e for e in self.events(root) if e["event"] == "sha256_mismatch"]
        self.assertEqual(len(mismatch), 1)
        self.assertEqual(mismatch[0]["expected_sha256"], bad)
        self.assertEqual(mismatch[0]["actual_sha256"], self.good_sha)
        self.assertEqual(mismatch[0]["level"], "ERROR")
        self.assertIn("sha256_mismatch", result.stderr)
        self.assertFalse((root / "ndk-r21b").exists())
        self.assertFalse((root / "downloads" / "android-ndk-r21b-linux-x86_64.zip").exists())
        self.assertEqual([p.name for p in root.iterdir() if p.name.startswith(".build-")], [])

    def test_a_clang_that_does_not_print_the_pinned_line_is_refused_and_leaves_nothing(self):
        root = self.tmp / "root3"
        wrong_zip = self.tmp / "wrong.zip"
        make_fixture_zip(wrong_zip, WRONG)
        sha = hashlib.sha256(wrong_zip.read_bytes()).hexdigest()
        result = self.run_script(root, self.pin_for("p3", wrong_zip, sha))
        self.assertEqual(result.returncode, 1)
        bad = [e for e in self.events(root) if e["event"] == "clang_version_mismatch"]
        self.assertEqual(len(bad), 1)
        self.assertEqual(bad[0]["expected"], PINNED)
        self.assertEqual(bad[0]["actual"], WRONG)
        self.assertFalse((root / "ndk-r21b").exists())
        self.assertEqual([p.name for p in root.iterdir() if p.name.startswith(".build-")], [])

    def test_an_installed_toolchain_whose_clang_changed_is_refused_on_the_next_run(self):
        root = self.tmp / "root4"
        pin = self.pin_for("p4", self.good_zip, self.good_sha)
        self.assertEqual(self.run_script(root, pin).returncode, 0)
        clang = root / "ndk-r21b" / CLANG_REL
        clang.write_text(f"#!/usr/bin/env bash\necho '{WRONG}'\n")
        again = self.run_script(root, pin)
        self.assertEqual(again.returncode, 1)
        self.assertIn("clang_version_mismatch", self.names(root))

    def test_a_cached_archive_whose_bytes_changed_is_refused_before_it_is_extracted(self):
        root = self.tmp / "root5"
        pin = self.pin_for("p5", self.good_zip, self.good_sha)
        self.assertEqual(self.run_script(root, pin).returncode, 0)
        # Remove the install so the next run must extract again, and corrupt the cached archive.
        subprocess.run(["rm", "-rf", str(root / "ndk-r21b")], check=True)
        (root / "downloads" / "android-ndk-r21b-linux-x86_64.zip").write_bytes(b"not the archive")
        before = len(self.events(root))
        again = self.run_script(root, pin)
        self.assertEqual(again.returncode, 1)
        later = [e["event"] for e in self.events(root)[before:]]
        self.assertIn("archive_cached", later)
        self.assertIn("sha256_mismatch", later)
        self.assertNotIn("extract_start", later)

    def test_a_download_that_fails_is_refused_and_named(self):
        root = self.tmp / "root6"
        result = self.run_script(root, self.pin_for("p6", self.tmp / "absent.zip", self.good_sha))
        self.assertEqual(result.returncode, 1)
        self.assertIn("download_failed", self.names(root))

    def test_an_archive_with_another_layout_is_refused(self):
        root = self.tmp / "root7"
        odd = self.tmp / "odd.zip"
        with zipfile.ZipFile(odd, "w") as zf:
            zf.writestr("android-ndk-r99/readme", "x")
        sha = hashlib.sha256(odd.read_bytes()).hexdigest()
        result = self.run_script(root, self.pin_for("p7", odd, sha))
        self.assertEqual(result.returncode, 1)
        self.assertIn("layout_unexpected", self.names(root))
        self.assertFalse((root / "ndk-r21b").exists())

    # --- the pin's own refusals ---

    def test_a_pin_with_an_unknown_key_or_a_missing_value_is_refused(self):
        root = self.tmp / "root8"
        unknown = self.pin_for("p8", self.good_zip, self.good_sha, extra="NDK_FLAVOUR=x\n")
        self.assertEqual(self.run_script(root, unknown).returncode, 1)
        self.assertIn("pin_unknown_key", self.names(root))
        for field in ("NDK_SHA256", "NDK_URL", "CLANG_VERSION", "NDK_RELEASE", "NDK_BUILD"):
            with self.subTest(empty=field):
                root_n = self.tmp / f"root8-{field}"
                lines = [line for line in self.pin_for("p8b", self.good_zip, self.good_sha).read_text().splitlines()]
                pin = self.tmp / f"p8-{field}.pin"
                pin.write_text("\n".join(f"{field}=" if line.startswith(field + "=") else line for line in lines) + "\n")
                self.assertEqual(self.run_script(root_n, pin).returncode, 1)
                self.assertIn("pin_incomplete", self.names(root_n))

    def test_a_missing_pin_is_refused(self):
        root = self.tmp / "root9"
        self.assertEqual(self.run_script(root, self.tmp / "nowhere.pin").returncode, 1)
        self.assertIn("pin_missing", self.names(root))

    # --- --print-dir ---

    def test_print_dir_names_the_install_directory_only_when_it_is_installed(self):
        root = self.tmp / "root10"
        pin = self.pin_for("p10", self.good_zip, self.good_sha)
        absent = self.run_script(root, pin, "--print-dir")
        self.assertEqual(absent.returncode, 1)
        self.assertEqual(absent.stdout, "")
        self.assertEqual(self.run_script(root, pin).returncode, 0)
        present = self.run_script(root, pin, "--print-dir")
        self.assertEqual(present.returncode, 0, present.stderr)
        self.assertEqual(present.stdout.strip(), str(root / "ndk-r21b"))

    def test_the_default_root_is_under_the_xdg_cache_directory(self):
        pin = self.pin_for("p11", self.good_zip, self.good_sha)
        cache = self.tmp / "xdg"
        env = {**os.environ, "XDG_CACHE_HOME": str(cache)}
        done = subprocess.run([sys.executable, "-I", str(SCRIPT), "--pin", str(pin)], capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertTrue((cache / "nevr-runtime" / "toolchain" / "ndk-r21b" / CLANG_REL).is_file())
        self.assertTrue((cache / "nevr-runtime" / "toolchain" / "ndk-install.log").is_file())

    # --- the recipe ---

    def test_the_recipe_runs_the_installer(self):
        text = (REPO / "justfile").read_text(encoding="utf-8")
        start = text.index("\ntoolchain-r21b:")
        recipe = text[start:text.index("\n\n", start)]
        self.assertIn("python3 -I tools/install_game_ndk.py", recipe)


if __name__ == "__main__":
    unittest.main()
