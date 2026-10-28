"""The library entries of the Quest repack (#1117): a plugin may not take a game library's place, and every library of the
store APK survives signing.

Two layers. The unit tests call tools/quest_repack_libs.py on small zip files made here. The end-to-end tests run
`just android-repack-apk` for real (the fixture of test_quest_repack_version.py: a fixture APK, a compiled shim, a
fixture key through the real tools/quest_keystore.sh with a fake `pass`) with an AArch64 plugin made by clang, and read the
SIGNED APK back with the standard zip reader.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "tests"))
import quest_repack_libs as libs  # noqa: E402
import work_root  # noqa: E402
from test_quest_repack_version import DEV_VERSION, JUSTFILE, MISSING, RepackFixture  # noqa: E402

LIBR15 = b"\x7fELF the game\0" * 50  # what RepackFixture.make_inputs puts under lib/arm64-v8a/libr15.so
AARCH64_MISSING = [t for t in ("clang", "ld.lld", "llvm-nm", "llvm-readelf") if shutil.which(t) is None]


def make_zip(path: pathlib.Path, entries: dict[str, bytes]) -> pathlib.Path:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return path


class PluginNameTest(unittest.TestCase):
    """What a plugin's file name collides with."""

    STORE = ["lib/arm64-v8a/libr15.so", "lib/arm64-v8a/libovrplatformloader.so", "lib/armeabi-v7a/libother.so"]

    def test_a_name_no_entry_has_collides_with_nothing(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libgood.so"), [])

    def test_a_store_library_name_collides(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libr15.so"), ["lib/arm64-v8a/libr15.so"])

    def test_the_name_collides_in_any_abi_directory(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libother.so"), ["lib/armeabi-v7a/libother.so"])

    def test_a_library_the_recipe_stages_collides_even_when_the_store_lacks_it(self):
        found = libs.plugin_name_collisions(self.STORE, "libovrplatformloader_orig.so")
        self.assertEqual(len(found), 1)
        self.assertIn("staged by the recipe", found[0])

    def test_the_loader_name_collides_with_the_store_entry_and_the_staged_shim(self):
        self.assertEqual(len(libs.plugin_name_collisions(self.STORE, "libovrplatformloader.so")), 2)

    def test_only_a_whole_file_name_collides(self):
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "libr15.so.1"), [])
        self.assertEqual(libs.plugin_name_collisions(self.STORE, "r15.so"), [])
        self.assertEqual(libs.plugin_name_collisions(["assets/libr15.so"], "libr15.so"), ["assets/libr15.so"])


class CheckPluginNameFromFileTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-libs-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)

    def test_the_store_apk_is_read_not_assumed(self):
        apk = make_zip(self.tmp / "store.apk", {"lib/arm64-v8a/libr15.so": b"x", "lib/arm64-v8a/libextra.so": b"y",
                                                "assets/a.bin": b"z"})
        self.assertEqual(libs.check_plugin_name(apk, self.tmp / "libextra.so"), ["lib/arm64-v8a/libextra.so"])
        self.assertEqual(libs.check_plugin_name(apk, self.tmp / "libfree.so"), [])
        self.assertEqual(libs.check_plugin_name(apk, self.tmp / "a.bin"), [])  # only lib/ entries count

    def test_the_command_line_exits_one_and_names_the_entry(self):
        apk = make_zip(self.tmp / "store.apk", {"lib/arm64-v8a/libr15.so": b"x"})
        plugin = self.tmp / "libr15.so"
        plugin.write_bytes(b"p")
        refused = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "check-plugin-name",
                                  "--apk", str(apk), "--plugin", str(plugin)], capture_output=True, text=True)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("plugin libr15.so would replace lib/arm64-v8a/libr15.so", refused.stderr)
        self.assertNotIn("OK", refused.stdout)
        plugin_ok = self.tmp / "libok.so"
        plugin_ok.write_bytes(b"p")
        accepted = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "check-plugin-name",
                                   "--apk", str(apk), "--plugin", str(plugin_ok)], capture_output=True, text=True)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("check-plugin-name OK", accepted.stdout)


class VerifySignedTest(unittest.TestCase):
    """The signed APK against the store APK and the staged files."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-libs-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.store = {
            "AndroidManifest.xml": b"manifest",
            "lib/arm64-v8a/libovrplatformloader.so": b"store loader",
            "lib/arm64-v8a/libr15.so": b"the game",
            "lib/arm64-v8a/libpnsovr.so": b"pns",
            "lib/armeabi-v7a/libold.so": b"old abi",
        }
        self.staged = {
            "libovrplatformloader.so": b"the shim",
            "libovrplatformloader_orig.so": b"store loader, renamed",
            "libplugin.so": b"a plugin",
        }
        self.signed = {
            **{k: v for k, v in self.store.items() if k != "lib/arm64-v8a/libovrplatformloader.so"},
            **{f"lib/arm64-v8a/{k}": v for k, v in self.staged.items()},
        }

    def run_verify(self) -> list[str]:
        source = make_zip(self.tmp / "store.apk", self.store)
        signed = make_zip(self.tmp / "signed.apk", self.signed)
        stage = self.tmp / "stage"
        shutil.rmtree(stage, ignore_errors=True)
        (stage / "lib" / "arm64-v8a").mkdir(parents=True)
        for name, data in self.staged.items():
            (stage / "lib" / "arm64-v8a" / name).write_bytes(data)
        return libs.verify_signed(source, signed, stage)

    def test_an_apk_where_everything_survived_has_no_problems(self):
        self.assertEqual(self.run_verify(), [])

    def test_the_loader_the_shim_replaces_may_differ(self):
        self.signed["lib/arm64-v8a/libovrplatformloader.so"] = b"the shim"
        self.assertEqual(self.run_verify(), [])

    def test_a_store_library_missing_from_the_signed_apk_is_named(self):
        del self.signed["lib/arm64-v8a/libr15.so"]
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("lib/arm64-v8a/libr15.so", problems[0])
        self.assertIn("missing from the signed APK", problems[0])

    def test_a_store_library_with_other_bytes_is_named(self):
        self.signed["lib/arm64-v8a/libpnsovr.so"] = b"a plugin took this name"
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("lib/arm64-v8a/libpnsovr.so", problems[0])
        self.assertIn("different bytes than the store APK", problems[0])

    def test_a_library_of_another_abi_is_held_to_the_same_rule(self):
        del self.signed["lib/armeabi-v7a/libold.so"]
        self.assertEqual(len(self.run_verify()), 1)

    def test_a_staged_library_missing_from_the_signed_apk_is_named(self):
        del self.signed["lib/arm64-v8a/libplugin.so"]
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("libplugin.so: staged, missing", problems[0])

    def test_a_staged_library_with_other_bytes_is_named(self):
        self.signed["lib/arm64-v8a/libovrplatformloader_orig.so"] = b"something else"
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("different bytes than the staged file", problems[0])

    def test_every_problem_is_reported_not_only_the_first(self):
        del self.signed["lib/arm64-v8a/libr15.so"]
        del self.signed["lib/arm64-v8a/libplugin.so"]
        self.assertEqual(len(self.run_verify()), 2)


@unittest.skipIf(MISSING or AARCH64_MISSING, f"needs {', '.join(MISSING + AARCH64_MISSING)}")
class RepackWithAPluginTest(RepackFixture):
    """`just android-repack-apk` with plugin=, end to end."""

    def plugin(self, name: str, source: str = "int plugin_marker = 1;\n") -> pathlib.Path:
        out = self.tmp / "plugins"
        out.mkdir(exist_ok=True)
        src = out / (name + ".c")
        src.write_text(source)
        path = out / name
        subprocess.run(["clang", "--target=aarch64-linux-gnu", "-shared", "-fPIC", "-nostdlib", "-fuse-ld=lld",
                        "-o", str(path), str(src)], check=True, capture_output=True)
        return path

    def repack_with(self, plugin: pathlib.Path):
        work = self.tmp / "work"
        result = subprocess.run(
            ["just", "--justfile", str(JUSTFILE), "--working-directory", str(REPO), "android-repack-apk",
             str(self.apk), str(self.shim), os.path.relpath(work, REPO), str(plugin)],
            capture_output=True, text=True, env=self.env, timeout=300)
        self.signed = work / "r15_nevr-sentinel_signed.apk"
        return result

    def add_store_entry(self, name: str, data: bytes) -> None:
        with zipfile.ZipFile(self.apk, "a") as z:
            z.writestr(name, data)

    def test_a_clean_plugin_is_packaged_and_the_game_libraries_survive(self):
        self.make_inputs(DEV_VERSION)
        plugin = self.plugin("libgood.so")
        result = self.repack_with(plugin)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify-signed OK", result.stdout)
        with zipfile.ZipFile(self.signed) as z:
            self.assertEqual(z.read("lib/arm64-v8a/libgood.so"), plugin.read_bytes())
            self.assertEqual(z.read("lib/arm64-v8a/libr15.so"), LIBR15)
            self.assertEqual(z.read("lib/arm64-v8a/libovrplatformloader_orig.so")[:4], b"\x7fELF")

    def test_a_plugin_named_like_a_game_library_is_refused_before_anything_is_signed(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libr15.so"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("plugin libr15.so would replace lib/arm64-v8a/libr15.so", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_named_like_a_library_the_recipe_stages_is_refused(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libovrplatformloader_orig.so"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staged by the recipe", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_named_like_a_library_of_another_abi_is_refused(self):
        self.make_inputs(DEV_VERSION)
        self.add_store_entry("lib/armeabi-v7a/libold.so", b"old")
        result = self.repack_with(self.plugin("libold.so"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lib/armeabi-v7a/libold.so", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_with_a_load_time_initializer_is_refused(self):
        self.make_inputs(DEV_VERSION)
        ctor = "__attribute__((constructor)) static void run(void) {}\nint plugin_marker = 2;\n"
        result = self.repack_with(self.plugin("libctor.so", ctor))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is not an expected initializer", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_repack_without_a_plugin_still_verifies_the_store_libraries(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify-signed OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
