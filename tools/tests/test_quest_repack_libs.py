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
import struct
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

AARCH64_MISSING = [t for t in ("clang", "ld.lld", "llvm-nm", "llvm-readelf") if shutil.which(t) is None]
GAME_ID = "b243509c08ce677aeb95fa348016949b3fc45230"  # the fixture libr15's build ID (the pinned one, kLibR15BuildId)
OTHER_ID = "0123456789abcdef0123456789abcdef01234567"


def note_macro() -> str:
    """The NEVR_DECLARE_SUPPORTED_BUILD_IDS definition, lifted from plugin_interface.h as it is: the header is C++ and
    the bare AArch64 target here has no C++ library to include it with."""
    lines = (REPO / "src" / "extension" / "plugin_interface.h").read_text().splitlines()
    start = next(i for i, l in enumerate(lines) if "#define NEVR_DECLARE_SUPPORTED_BUILD_IDS(" in l)
    end = start
    while lines[end].rstrip().endswith("\\"):
        end += 1
    return '#define NEVR_NOTE_SECTION_TYPE "%note"\n' + "\n".join(lines[start:end + 1]) + "\n"


def nevr_note(ids: str) -> str:
    """C source for a plugin's NEVR_DECLARE_SUPPORTED_BUILD_IDS note, from the real macro."""
    return f'{note_macro()}NEVR_DECLARE_SUPPORTED_BUILD_IDS("{ids}");\n'


def build_aarch64(out: pathlib.Path, source: str, *, build_id: str | None = None) -> pathlib.Path:
    """An AArch64 shared object from `source`; with `build_id`, carrying that GNU build ID, else none."""
    out.parent.mkdir(parents=True, exist_ok=True)
    src = out.with_suffix(out.suffix + ".c")
    src.write_text(source)
    link_id = f"-Wl,--build-id=0x{build_id}" if build_id else "-Wl,--build-id=none"
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-shared", "-fPIC", "-nostdlib", "-fuse-ld=lld", link_id,
                    "-o", str(out), str(src)], check=True, capture_output=True)
    return out


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


@unittest.skipIf(AARCH64_MISSING, f"needs {', '.join(AARCH64_MISSING)}")
class PluginBuildIdsTest(unittest.TestCase):
    """The loader's build-ID rule (plugin_image.h), checked against the store APK's libr15.so before signing."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-ids-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.libr15 = build_aarch64(self.tmp / "libr15.so", "int game = 1;\n", build_id=GAME_ID).read_bytes()

    def problems(self, source: str, libr15: bytes = None) -> list[str]:
        plugin = build_aarch64(self.tmp / "libp.so", source).read_bytes()
        return libs.build_id_problems(self.libr15 if libr15 is None else libr15, plugin, "libp.so")

    def test_a_plugin_listing_the_game_build_passes(self):
        self.assertEqual(self.problems(nevr_note(GAME_ID)), [])

    def test_the_game_build_anywhere_in_the_list_passes(self):
        self.assertEqual(self.problems(nevr_note(f"{OTHER_ID}, {OTHER_ID[:-1]}9\\t{GAME_ID}")), [])

    def test_a_plugin_with_no_note_is_named_undeclared(self):
        found = self.problems("int plugin_marker = 1;\n")
        self.assertEqual(len(found), 1)
        self.assertIn("build_ids_undeclared", found[0])

    def test_a_list_without_the_game_build_is_named_unsupported(self):
        found = self.problems(nevr_note(OTHER_ID))
        self.assertEqual(len(found), 1)
        self.assertIn("build_id_unsupported", found[0])
        self.assertIn(GAME_ID, found[0])

    def test_a_prefix_of_the_game_build_is_not_the_game_build(self):
        self.assertIn("build_id_unsupported", self.problems(nevr_note(GAME_ID[:-2]))[0])
        self.assertIn("build_id_unsupported", self.problems(nevr_note(GAME_ID + "00"))[0])

    def test_an_empty_list_is_unsupported(self):
        self.assertIn("an empty list", self.problems(nevr_note(""))[0])

    def test_a_libr15_with_no_build_id_checks_nothing(self):
        bare = build_aarch64(self.tmp / "bare.so", "int game = 1;\n").read_bytes()
        found = self.problems(nevr_note(GAME_ID), libr15=bare)
        self.assertEqual(len(found), 1)
        self.assertIn("has no GNU build ID", found[0])

    def test_a_file_that_is_not_a_64_bit_elf_has_no_notes(self):
        self.assertIsNone(libs.note_desc(b"\x7fELF the game\0" * 50, b"GNU\0", libs.NT_GNU_BUILD_ID))
        self.assertIsNone(libs.note_desc(b"", b"GNU\0", libs.NT_GNU_BUILD_ID))

    def test_the_build_id_is_read_as_readelf_reads_it(self):
        out = subprocess.run(["llvm-readelf", "-n", str(self.tmp / "libr15.so")], capture_output=True, text=True,
                             check=True).stdout
        self.assertIn(f"Build ID: {GAME_ID}", out)
        self.assertEqual(libs.note_desc(self.libr15, b"GNU\0", libs.NT_GNU_BUILD_ID).hex(), GAME_ID)

    def test_the_command_reads_the_store_apk_libr15(self):
        plugin = build_aarch64(self.tmp / "libp.so", nevr_note(OTHER_ID))
        apk = make_zip(self.tmp / "store.apk", {"lib/arm64-v8a/libr15.so": self.libr15})
        self.assertIn("build_id_unsupported", libs.check_plugin_build_ids(apk, plugin)[0])
        result = subprocess.run([sys.executable, str(REPO / "tools" / "quest_repack_libs.py"), "check-plugin-build-ids",
                                 "--apk", str(apk), "--plugin", str(plugin)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("build_id_unsupported", result.stderr)
        good = build_aarch64(self.tmp / "libg.so", nevr_note(GAME_ID))
        result = subprocess.run([sys.executable, str(REPO / "tools" / "quest_repack_libs.py"), "check-plugin-build-ids",
                                 "--apk", str(apk), "--plugin", str(good)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("check-plugin-build-ids OK", result.stdout)

    def test_a_store_apk_without_libr15_is_named(self):
        plugin = build_aarch64(self.tmp / "libp.so", nevr_note(GAME_ID))
        apk = make_zip(self.tmp / "store.apk", {"lib/arm64-v8a/libother.so": b"x"})
        self.assertEqual(libs.check_plugin_build_ids(apk, plugin), ["the store APK has no lib/arm64-v8a/libr15.so"])


def note(owner: bytes, n_type: int, desc: bytes) -> bytes:
    pad = lambda b: b + b"\0" * (-len(b) % 4)  # noqa: E731
    return struct.pack("<III", len(owner), len(desc), n_type) + pad(owner) + pad(desc)


def elf(segments: list[tuple[int, bytes]], *, phentsize: int = 56, phnum: int | None = None) -> bytes:
    """A 64-bit little-endian ELF header, its program headers, and each segment's bytes after them, in order."""
    count = len(segments) if phnum is None else phnum
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", header, 0x20, 64)
    struct.pack_into("<HH", header, 0x36, phentsize, count)
    offset, phdrs, data = 64 + 56 * len(segments), b"", b""
    for p_type, body in segments:
        phdrs += struct.pack("<IIQQQQQQ", p_type, 4, offset + len(data), 0, 0, len(body), len(body), 4)
        data += body
    return bytes(header) + phdrs + data


class NoteWalkTest(unittest.TestCase):
    """note_desc walks the PT_NOTE segments as plugin_image.cpp does, and stops at a header that does not hold."""

    BUILD_ID = bytes.fromhex(GAME_ID)

    def read(self, image: bytes):
        return libs.note_desc(image, b"GNU\0", libs.NT_GNU_BUILD_ID)

    def test_the_build_id_after_other_notes_is_found(self):
        notes = note(b"GNU\0", 1, b"\0" * 16) + note(b"NEVR\0", 1, b"x") + note(b"GNU\0", 3, self.BUILD_ID)
        self.assertEqual(self.read(elf([(libs.PT_NOTE, notes)])), self.BUILD_ID)

    def test_a_note_segment_after_another_segment_is_read(self):
        image = elf([(1, b"\0" * 32), (libs.PT_NOTE, note(b"GNU\0", 3, self.BUILD_ID))])
        self.assertEqual(self.read(image), self.BUILD_ID)

    def test_a_note_that_ends_the_file_is_read(self):
        image = elf([(libs.PT_NOTE, note(b"GNU\0", 3, self.BUILD_ID))])
        self.assertTrue(image.endswith(self.BUILD_ID))
        self.assertEqual(self.read(image), self.BUILD_ID)

    def test_note_bytes_outside_a_note_segment_are_not_a_note(self):
        self.assertIsNone(self.read(elf([(1, note(b"GNU\0", 3, self.BUILD_ID))])))

    def test_a_segment_that_runs_past_the_file_is_skipped(self):
        image = elf([(libs.PT_NOTE, note(b"GNU\0", 3, self.BUILD_ID))])
        self.assertIsNone(self.read(image[:-1]))

    def test_program_headers_past_the_file_or_too_small_are_refused(self):
        body = note(b"GNU\0", 3, self.BUILD_ID)
        self.assertIsNone(self.read(elf([(libs.PT_NOTE, body)], phnum=200)))
        self.assertIsNone(self.read(elf([(libs.PT_NOTE, body)], phentsize=55)))

    def test_a_description_that_runs_past_its_segment_is_not_returned(self):
        whole = note(b"GNU\0", 3, self.BUILD_ID)
        cut = struct.pack("<III", 4, 64, 3) + whole[12:]
        self.assertIsNone(self.read(elf([(libs.PT_NOTE, cut)])))

    def test_a_name_that_runs_past_its_segment_stops_the_walk(self):
        self.assertIsNone(self.read(elf([(libs.PT_NOTE, struct.pack("<III", 64, 0, 3) + b"GNU\0")])))

    def test_another_class_or_byte_order_has_no_notes(self):
        image = bytearray(elf([(libs.PT_NOTE, note(b"GNU\0", 3, self.BUILD_ID))]))
        for at, value in ((4, 1), (5, 2), (0, 0)):
            bad = bytearray(image)
            bad[at] = value
            self.assertIsNone(self.read(bytes(bad)), at)


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

    def make_inputs(self, version: str, **kw):
        """The fixture APK, with its libr15.so a real AArch64 object carrying GAME_ID (the build-ID check reads it)."""
        super().make_inputs(version, **kw)
        self.libr15 = build_aarch64(self.tmp / "game" / "libr15.so", "int game = 1;\n", build_id=GAME_ID).read_bytes()
        rebuilt = self.tmp / "store-rebuilt.apk"
        with zipfile.ZipFile(self.apk) as old, zipfile.ZipFile(rebuilt, "w", zipfile.ZIP_DEFLATED) as new:
            for info in old.infolist():
                data = self.libr15 if info.filename == "lib/arm64-v8a/libr15.so" else old.read(info)
                new.writestr(info, data)
        rebuilt.replace(self.apk)

    def plugin(self, name: str, source: str = "int plugin_marker = 1;\n", ids: str | None = GAME_ID) -> pathlib.Path:
        """A plugin from `source`, declaring `ids` in its NEVR note (None: no note)."""
        return build_aarch64(self.tmp / "plugins" / name, (nevr_note(ids) if ids is not None else "") + source)

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
            self.assertEqual(z.read("lib/arm64-v8a/libr15.so"), self.libr15)
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

    def test_a_plugin_that_declares_no_build_ids_is_refused_before_anything_is_signed(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libnonote.so", ids=None))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build_ids_undeclared", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_for_another_game_build_is_refused_before_anything_is_signed(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libother.so", ids=OTHER_ID))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build_id_unsupported", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_repack_without_a_plugin_still_verifies_the_store_libraries(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify-signed OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
