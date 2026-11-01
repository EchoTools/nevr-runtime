"""The library entries of the Quest repack (#1117): a plugin may not take a game library's place, and every library of the
store APK survives signing.

Two layers. The unit tests call tools/quest_repack_libs.py on small zip files made here. The end-to-end tests run
`just android-repack-apk` for real (the fixture of test_quest_repack_version.py: a fixture APK, a compiled shim, a
fixture key through the real tools/quest_keystore.sh, named by NEVR_QUEST_KEYSTORE and NEVR_QUEST_KEYSTORE_PASS) with an
AArch64 plugin made by clang, and read the
SIGNED APK back with the standard zip reader.
"""
from __future__ import annotations

import hashlib
import json
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
from quest_patcher_standin import MARKER, make_standin, recorded_argv, recorded_runs  # noqa: E402
from test_quest_repack_version import DEV_VERSION, JUSTFILE, MISSING, RepackFixture  # noqa: E402
from test_quest_runtime_pin import OLD as RUNTIME_STOCK_MD5, SECOND as RUNTIME_SECOND_MD5, make_elf, make_runtime  # noqa: E402
import quest_runtime_pin as qrp  # noqa: E402

LIBR15 = b"\x7fELF the game\0" * 50  # what RepackFixture.make_inputs puts under lib/arm64-v8a/libr15.so
PATCHED_LIBR15 = LIBR15 + MARKER  # what the stand-in patcher (quest_patcher_standin.py) makes of it
RUNTIME = "lib/arm64-v8a/libvrapi.so"
STORE_VRAPI = make_elf(b"\0" * 8, b"the store's own libvrapi, no md5 list\0")  # a libvrapi that carries no checksum list
AARCH64_MISSING = [t for t in ("clang", "ld.lld", "llvm-nm", "llvm-readelf") if shutil.which(t) is None]
GAME_ID = "b243509c08ce677aeb95fa348016949b3fc45230"  # the fixture libr15's build ID (the pinned one, kLibR15BuildId)
OTHER_ID = "0123456789abcdef0123456789abcdef01234567"


def note_macro() -> str:
    """The NEVR_DECLARE_SUPPORTED_BUILD_IDS definition, lifted from plugin_interface.h as it is: the header is C++ and
    the bare AArch64 target here has no C++ library to include it with."""
    lines = (REPO / "sdk" / "include" / "nevr" / "plugin_interface.h").read_text().splitlines()
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
    """The signed APK against the store APK, the staged files and the patcher's output (ADR 0021 section 9, test 7).

    The patcher is a stand-in (quest_patcher_standin.py): it makes `store bytes + MARKER` of whatever it is given."""

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
            "libr15.so": b"the game" + MARKER,
            "libplugin.so": b"a plugin",
        }
        self.signed = {
            **{k: v for k, v in self.store.items() if k not in ("lib/arm64-v8a/libovrplatformloader.so", "lib/arm64-v8a/libr15.so")},
            **{f"lib/arm64-v8a/{k}": v for k, v in self.staged.items()},
        }
        self.patcher = make_standin(self.tmp / "bin", "ok")
        self.extra_rows = None

    def run_verify(self) -> list[str]:
        source = make_zip(self.tmp / "store.apk", self.store)
        signed = make_zip(self.tmp / "signed.apk", self.signed)
        stage = self.tmp / "stage"
        shutil.rmtree(stage, ignore_errors=True)
        (stage / "lib" / "arm64-v8a").mkdir(parents=True)
        for name, data in self.staged.items():
            (stage / "lib" / "arm64-v8a" / name).write_bytes(data)
        return libs.verify_signed(source, signed, stage, self.patcher, self.extra_rows, self.tmp)

    def run_cli(self):
        """The same check through the command line (exit code, stdout, stderr)."""
        make_zip(self.tmp / "store.apk", self.store)
        make_zip(self.tmp / "signed.apk", self.signed)
        stage = self.tmp / "stage"
        shutil.rmtree(stage, ignore_errors=True)
        (stage / "lib" / "arm64-v8a").mkdir(parents=True)
        for name, data in self.staged.items():
            (stage / "lib" / "arm64-v8a" / name).write_bytes(data)
        command = [sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "verify-signed",
                   "--source", str(self.tmp / "store.apk"), "--signed", str(self.tmp / "signed.apk"), "--stage", str(stage),
                   "--patcher", str(self.patcher), "--scratch", str(self.tmp)]
        if self.extra_rows is not None:
            command += ["--extra-rows", str(self.extra_rows)]
        return subprocess.run(command, capture_output=True, text=True)

    def test_an_apk_where_everything_survived_has_no_problems(self):
        self.assertEqual(self.run_verify(), [])

    def add_manifest(self, plugins: list[dict]) -> None:
        """The staged manifest, and the signed APK holding the same bytes (the signed APK must hold what was staged)."""
        text = json.dumps({"schema": 1, "plugins": plugins}, sort_keys=True, separators=(",", ":")) + "\n"
        self.staged["libnevr_plugin_manifest.so"] = text.encode()
        self.signed["lib/arm64-v8a/libnevr_plugin_manifest.so"] = text.encode()

    def test_verify_signed_refuses_a_manifest_whose_hash_is_not_the_staged_plugins(self):
        self.add_manifest([{"name": "plugin", "file": "libplugin.so", "enabled": True, "early": False,
                            "sha256": "0" * 64}])
        problems = self.run_verify()
        self.assertTrue(any("libnevr_plugin_manifest.so" in p and "sha256" in p for p in problems), problems)

    def test_verify_signed_refuses_a_staged_plugin_the_manifest_does_not_list(self):
        self.add_manifest([])
        problems = self.run_verify()
        self.assertTrue(any("libplugin.so" in p and "manifest" in p for p in problems), problems)

    def test_the_loader_the_shim_replaces_may_differ(self):
        self.signed["lib/arm64-v8a/libovrplatformloader.so"] = b"the shim"
        self.assertEqual(self.run_verify(), [])

    def test_a_store_library_missing_from_the_signed_apk_is_named(self):
        del self.signed["lib/arm64-v8a/libpnsovr.so"]
        problems = self.run_verify()
        self.assertEqual(len(problems), 1)
        self.assertIn("lib/arm64-v8a/libpnsovr.so", problems[0])
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
        del self.signed["lib/arm64-v8a/libpnsovr.so"]
        del self.signed["lib/arm64-v8a/libplugin.so"]
        self.assertEqual(len(self.run_verify()), 2)

    # ---- libr15.so is the patcher's output (ADR 0021 test 7) ------------------------------------------------------------

    def only_problem(self) -> str:
        problems = self.run_verify()
        self.assertEqual(len(problems), 1, problems)
        return problems[0]

    def digest(self, data: bytes) -> str:
        return hashlib.sha256(data).hexdigest()

    def test_the_signed_libr15_is_the_patchers_output_and_the_command_line_says_ok(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("verify-signed OK", result.stdout)

    def test_a_signed_libr15_that_is_the_store_bytes_is_refused(self):
        self.signed["lib/arm64-v8a/libr15.so"] = self.store["lib/arm64-v8a/libr15.so"]
        self.staged["libr15.so"] = self.store["lib/arm64-v8a/libr15.so"]
        problem = self.only_problem()
        self.assertIn("lib/arm64-v8a/libr15.so: the signed APK does not hold the patcher's output", problem)
        self.assertIn(self.digest(b"the game"), problem)  # the signed (= store) digest is named
        result = self.run_cli()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("OK", result.stdout)

    def test_a_signed_libr15_made_by_the_patcher_from_another_input_is_refused(self):
        other = b"another libr15" + MARKER  # what the patcher makes of a different store file
        self.signed["lib/arm64-v8a/libr15.so"] = other
        self.staged["libr15.so"] = other
        problem = self.only_problem()
        self.assertIn("does not hold the patcher's output", problem)
        self.assertIn(self.digest(other), problem)
        self.assertIn(self.digest(b"the game" + MARKER), problem)

    def test_a_patcher_that_exits_non_zero_is_refused_with_its_message(self):
        self.patcher = make_standin(self.tmp / "bin-fail", "fail")
        problem = self.only_problem()
        self.assertIn("exited 3", problem)
        self.assertIn("standin patcher: refused", problem)
        self.assertEqual(self.run_cli().returncode, 1)

    def test_a_patcher_that_writes_no_file_is_refused(self):
        self.patcher = make_standin(self.tmp / "bin-nowrite", "nowrite")
        self.assertIn("exited 0 and wrote no file", self.only_problem())

    def test_a_patcher_that_writes_an_empty_file_is_refused(self):
        self.patcher = make_standin(self.tmp / "bin-empty", "empty")
        self.assertIn("exited 0 and wrote no file", self.only_problem())

    def test_a_patcher_that_is_not_there_is_refused(self):
        self.patcher = self.tmp / "no-such-patcher"
        self.assertIn("did not run to its end", self.only_problem())
        self.assertEqual(self.run_cli().returncode, 1)

    def test_a_patcher_whose_output_is_the_store_file_is_refused(self):
        self.patcher = make_standin(self.tmp / "bin-same", "same")
        self.signed["lib/arm64-v8a/libr15.so"] = b"the game"
        self.staged["libr15.so"] = b"the game"
        self.assertIn("nothing was patched", self.only_problem())

    def test_a_libr15_missing_from_the_signed_apk_is_refused_by_name(self):
        del self.signed["lib/arm64-v8a/libr15.so"]
        del self.staged["libr15.so"]
        problem = self.only_problem()
        self.assertIn("lib/arm64-v8a/libr15.so", problem)
        self.assertIn("missing from the signed APK", problem)
        self.assertEqual(self.run_cli().returncode, 1)

    def test_a_store_apk_without_libr15_is_refused(self):
        del self.store["lib/arm64-v8a/libr15.so"]
        problems = self.run_verify()
        self.assertTrue(any("missing from the store APK" in p for p in problems), problems)

    def test_the_three_digests_are_printed_on_success_and_on_refusal(self):
        for refuse in (False, True):
            if refuse:
                self.signed["lib/arm64-v8a/libr15.so"] = b"the game"
            result = self.run_cli()
            self.assertEqual(result.returncode, 1 if refuse else 0, result.stderr)
            self.assertIn(f"sha256 store   {self.digest(b'the game')}", result.stdout)
            self.assertIn(f"sha256 patched {self.digest(b'the game' + MARKER)}", result.stdout)
            signed = b"the game" if refuse else b"the game" + MARKER
            self.assertIn(f"sha256 signed  {self.digest(signed)}", result.stdout)

    def test_the_patcher_is_given_the_store_file_and_the_extra_rows_file(self):
        rows = self.tmp / "rows.json"
        rows.write_text("{}")
        self.extra_rows = rows
        self.assertEqual(self.run_verify(), [])
        argv = recorded_argv(self.patcher)
        flags = dict(zip(argv[0::2], argv[1::2]))
        self.assertEqual(sorted(flags), ["--extra-rows", "--out", "--report", "--stock"])
        self.assertEqual(flags["--extra-rows"], str(rows))

    def test_without_extra_rows_the_patcher_is_given_none(self):
        self.assertEqual(self.run_verify(), [])
        self.assertNotIn("--extra-rows", recorded_argv(self.patcher))

    def test_the_patcher_runs_on_a_copy_of_the_store_file_in_a_directory_that_is_removed(self):
        before = set(self.tmp.iterdir())
        self.assertEqual(self.run_verify(), [])
        argv = recorded_argv(self.patcher)
        stock = pathlib.Path(dict(zip(argv[0::2], argv[1::2]))["--stock"])
        self.assertEqual(stock.parent.parent, self.tmp)  # under --scratch, not a fixed path
        self.assertFalse(stock.parent.exists())
        new = set(self.tmp.iterdir()) - before
        self.assertEqual({p.name for p in new}, {"store.apk", "signed.apk", "stage"})

    def test_the_command_line_passes_the_extra_rows_and_the_scratch_directory_to_the_patcher(self):
        rows = self.tmp / "rows.json"
        rows.write_text("{}")
        self.extra_rows = rows
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = recorded_argv(self.patcher)
        flags = dict(zip(argv[0::2], argv[1::2]))
        self.assertEqual(flags["--extra-rows"], str(rows))
        self.assertEqual(pathlib.Path(flags["--stock"]).parent.parent, self.tmp)  # --scratch

    def test_the_command_line_without_a_patcher_is_a_usage_error_not_a_pass(self):
        make_zip(self.tmp / "store.apk", self.store)
        make_zip(self.tmp / "signed.apk", self.signed)
        (self.tmp / "stage" / "lib" / "arm64-v8a").mkdir(parents=True)
        result = subprocess.run([sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "verify-signed",
                                 "--source", str(self.tmp / "store.apk"), "--signed", str(self.tmp / "signed.apk"),
                                 "--stage", str(self.tmp / "stage")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("--patcher", result.stderr)
        self.assertNotIn("OK", result.stdout)

    def test_a_patcher_that_does_not_end_is_stopped_and_refused(self):
        self.patcher = make_standin(self.tmp / "bin-hang", "hang")
        saved = libs.PATCHER_TIMEOUT_SECONDS
        libs.PATCHER_TIMEOUT_SECONDS = 1
        self.addCleanup(setattr, libs, "PATCHER_TIMEOUT_SECONDS", saved)
        self.assertIn("did not run to its end", self.only_problem())

    def test_the_signed_libr15_is_compared_with_the_patcher_run_on_this_store_file(self):
        # the digest the signed entry is compared with comes from a run on THIS store file: the stand-in appends the
        # marker to whatever it is given, so a signed libr15 made from a different input never matches
        self.store["lib/arm64-v8a/libr15.so"] = b"a different store libr15"
        problem = self.only_problem()
        self.assertIn("does not hold the patcher's output", problem)


    def test_a_patcher_given_as_a_dot_slash_path_is_run_as_that_file_not_looked_up_on_path(self):
        # pathlib drops the leading "./" when it turns the argument back into text, and a name with no slash is searched on
        # PATH: the file named must be the file run, from any directory the command is started in
        here = pathlib.Path.cwd()
        os.chdir(self.patcher.parent)
        self.addCleanup(os.chdir, here)
        self.patcher = pathlib.Path("./" + self.patcher.name)
        self.assertEqual(self.run_verify(), [])
        self.assertEqual(len(recorded_runs(self.patcher.resolve())), 1)

    # ---- the reference runtime's libvrapi.so is pinned to the signed libr15.so (#1737) ----------------------------------

    def use_runtime(self, copy: bytes | None = None) -> bytes:
        """The source APK carries the reference runtime; the staged and signed copy is `copy`, by default the runtime
        pinned to the md5 of the staged libr15.so. Returns that copy."""
        self.store[RUNTIME] = make_runtime()
        if copy is None:
            copy = make_runtime(hashlib.md5(self.staged["libr15.so"]).hexdigest())
        self.staged["libvrapi.so"] = copy
        self.signed[RUNTIME] = copy
        return copy

    def test_a_runtime_pinned_to_the_signed_libr15_passes_and_its_digests_are_printed(self):
        copy = self.use_runtime()
        self.assertEqual(self.run_verify(), [])
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("verify-signed OK", result.stdout)
        self.assertIn(f"libvrapi.so sha256 source  {self.digest(make_runtime())}", result.stdout)
        self.assertIn(f"libvrapi.so sha256 signed  {self.digest(copy)}", result.stdout)

    def test_an_unpinned_runtime_is_refused_by_name(self):
        self.use_runtime(make_runtime())
        problem = self.only_problem()
        self.assertIn("lib/arm64-v8a/libvrapi.so: the signed APK holds the source APK's runtime unpinned", problem)
        self.assertIn(hashlib.md5(self.staged["libr15.so"]).hexdigest(), problem)
        self.assertEqual(self.run_cli().returncode, 1)

    def test_a_runtime_pinned_to_another_libr15_is_refused(self):
        self.use_runtime(make_runtime(hashlib.md5(b"another libr15").hexdigest()))
        self.assertIn("not hold the source APK's runtime pinned to the signed libr15.so", self.only_problem())

    def test_a_runtime_pinned_to_the_store_libr15_is_refused(self):
        self.use_runtime(make_runtime(hashlib.md5(b"the game").hexdigest()))  # the md5 of the unpatched file
        self.assertIn("not hold the source APK's runtime pinned to the signed libr15.so", self.only_problem())

    def test_a_runtime_missing_from_the_signed_apk_is_refused(self):
        self.use_runtime()
        del self.signed[RUNTIME]
        del self.staged["libvrapi.so"]
        problem = self.only_problem()
        self.assertIn("lib/arm64-v8a/libvrapi.so: the pinned runtime belongs here, missing from the signed APK", problem)

    def test_a_runtime_that_cannot_be_pinned_is_refused_with_the_reason(self):
        self.use_runtime()
        self.store[RUNTIME] = make_runtime(sites=11)
        problem = self.only_problem()
        self.assertIn("cannot be pinned to the signed libr15.so", problem)
        self.assertIn("44 immediate runs", problem)

    def test_a_source_runtime_whose_first_entry_is_damaged_is_refused_not_pinned_elsewhere(self):
        self.use_runtime()
        self.store[RUNTIME] = make_runtime().replace(RUNTIME_STOCK_MD5.encode(), b"x" * 32, 1)
        problem = self.only_problem()
        self.assertIn("cannot be pinned to the signed libr15.so", problem)
        self.assertIn(f"is {RUNTIME_SECOND_MD5}, expected {RUNTIME_STOCK_MD5}", problem)

    def test_a_runtime_with_another_entry_replaced_is_refused(self):
        md5 = hashlib.md5(self.staged["libr15.so"]).hexdigest()
        wrong, _ = qrp.pin_data(make_runtime(), md5, 1, RUNTIME_SECOND_MD5)  # the pin's own tool, aimed at index 1
        self.use_runtime(wrong)
        self.assertIn("not hold the source APK's runtime pinned to the signed libr15.so", self.only_problem())

    def test_a_libvrapi_with_no_checksum_list_is_held_to_the_store_bytes(self):
        self.store[RUNTIME] = STORE_VRAPI
        self.signed[RUNTIME] = STORE_VRAPI
        self.assertEqual(self.run_verify(), [])
        self.signed[RUNTIME] = STORE_VRAPI + b"x"
        problem = self.only_problem()
        self.assertIn("lib/arm64-v8a/libvrapi.so: the signed APK holds different bytes than the store APK", problem)

    def test_a_pinned_runtime_staged_beside_a_plugin_manifest_is_not_an_unlisted_plugin(self):
        self.use_runtime()
        self.add_manifest([{"name": "plugin", "file": "libplugin.so", "enabled": True, "early": False,
                            "sha256": hashlib.sha256(self.staged["libplugin.so"]).hexdigest()}])
        self.assertEqual(self.run_verify(), [])


class WritePluginManifestTest(unittest.TestCase):
    """`write_plugin_manifest` hashes the staged copy, the bytes the APK will carry, not the file the recipe was given."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-manifest-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.stage = self.tmp / "stage"
        (self.stage / "lib" / "arm64-v8a").mkdir(parents=True)

    def test_the_hash_is_of_the_staged_bytes_not_of_the_file_the_recipe_was_given(self):
        source = self.tmp / "libfx.so"
        source.write_bytes(b"the file the recipe was given")
        (self.stage / "lib" / "arm64-v8a" / "libfx.so").write_bytes(b"the bytes staged for the APK")
        self.assertEqual(libs.write_plugin_manifest(self.stage, [source], [], []), [])
        written = json.loads((self.stage / "lib" / "arm64-v8a" / libs.MANIFEST_NAME).read_bytes())
        self.assertEqual(written["plugins"][0]["sha256"], hashlib.sha256(b"the bytes staged for the APK").hexdigest())
        self.assertNotEqual(written["plugins"][0]["sha256"], hashlib.sha256(b"the file the recipe was given").hexdigest())


class WritePluginManifestGoldenTest(unittest.TestCase):
    """The writer's bytes are the golden file the Quest loader's parser test reads (src/quest/tests/packaged_plugins_test.cpp,
    ManifestParseReadsTheBytesTheRepackWriterWrote): the writer cannot change a key without this failing, and the parser cannot
    stop reading the golden file without that test failing."""

    GOLDEN = REPO / "src" / "quest" / "tests" / "fixtures" / "packaged_plugins_manifest.golden"

    def test_the_writers_output_for_two_plugins_is_the_golden_file(self):
        with tempfile.TemporaryDirectory(prefix="quest-repack-golden-", dir=work_root.test_scratch()) as tmp:
            root = pathlib.Path(tmp)
            stage = root / "stage"
            (stage / "lib" / "arm64-v8a").mkdir(parents=True)
            plugins = []
            for name, data in (("libalpha.so", b"alpha plugin\n"), ("libbeta.so", b"beta plugin\n")):
                (stage / "lib" / "arm64-v8a" / name).write_bytes(data)
                plugins.append(root / name)
            self.assertEqual(libs.write_plugin_manifest(stage, plugins, ["libalpha.so"], ["libbeta.so"]), [])
            written = (stage / "lib" / "arm64-v8a" / libs.MANIFEST_NAME).read_bytes()
        self.assertEqual(written, self.GOLDEN.read_bytes())


class ShimRowsTest(unittest.TestCase):
    """`check-shim-rows`: the shim is the build for the rows the library is patched with (ADR 0021 section 11)."""

    ROWS = b'{"format": 1}\n'
    OTHER_ROWS = b'{"format": 2}\n'

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-repack-shim-rows-", dir=work_root.test_scratch())
        self.addCleanup(self._tmp.cleanup)
        self.tmp = pathlib.Path(self._tmp.name)
        self.rows = self.tmp / "rows.json"
        self.rows.write_bytes(self.ROWS)
        self.other = self.tmp / "other.json"
        self.other.write_bytes(self.OTHER_ROWS)

    def shim(self, *parts: bytes) -> pathlib.Path:
        path = self.tmp / "libovrplatformloader.so"
        path.write_bytes(b"\x7fELF public rodata\0" + b"".join(parts))
        return path

    @staticmethod
    def sha(data: bytes) -> bytes:
        return hashlib.sha256(data).hexdigest().encode() + b"\0"

    def run_cli(self, shim: pathlib.Path, rows: pathlib.Path | None):
        command = [sys.executable, "-I", str(REPO / "tools" / "quest_repack_libs.py"), "check-shim-rows", "--shim", str(shim)]
        if rows is not None:
            command += ["--extra-rows", str(rows)]
        return subprocess.run(command, capture_output=True, text=True)

    def test_a_public_shim_with_no_rows_passes(self):
        self.assertEqual(libs.shim_rows_problems(self.shim(), None), [])
        self.assertEqual(self.run_cli(self.shim(), None).returncode, 0)

    def test_a_variant_shim_with_no_rows_is_refused(self):
        shim = self.shim(b"\0libr15_extra_rows\0", self.sha(self.ROWS))
        [problem] = libs.shim_rows_problems(shim, None)
        self.assertIn("variant build", problem)
        result = self.run_cli(shim, None)
        self.assertEqual(result.returncode, 1)
        self.assertIn("build_id_mismatch", result.stderr)

    def test_a_shim_built_with_the_rows_file_passes(self):
        shim = self.shim(b"\0libr15_extra_rows\0", self.sha(self.ROWS))
        self.assertEqual(libs.shim_rows_problems(shim, self.rows), [])
        self.assertEqual(self.run_cli(shim, self.rows).returncode, 0)

    def test_a_public_shim_with_a_rows_file_is_refused(self):
        [problem] = libs.shim_rows_problems(self.shim(), self.rows)
        self.assertIn("was not built with rows.json", problem)
        self.assertIn(hashlib.sha256(self.ROWS).hexdigest(), problem)
        self.assertEqual(self.run_cli(self.shim(), self.rows).returncode, 1)

    def test_a_shim_built_with_other_rows_is_refused(self):
        shim = self.shim(b"\0libr15_extra_rows\0", self.sha(self.OTHER_ROWS))
        self.assertEqual(len(libs.shim_rows_problems(shim, self.rows)), 1)

    def test_only_the_whole_digest_counts(self):
        # a digest cut short (no terminating NUL where the string ends) is not the embedded kSha256
        shim = self.shim(hashlib.sha256(self.ROWS).hexdigest().encode()[:-1] + b"\0")
        self.assertEqual(len(libs.shim_rows_problems(shim, self.rows)), 1)

    def test_a_shim_that_cannot_be_read_is_refused(self):
        self.assertEqual(self.run_cli(self.tmp / "missing.so", None).returncode, 1)


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

    def repack_with(self, *plugins: pathlib.Path, early: str = "", off: str = ""):
        work = self.tmp / "work"
        # The recipe's arguments in order: apk shim workdir plugin version extra_rows patcher json_include plugin_early plugin_off.
        # The last two are passed only when a test names them, so the older calls keep their seven arguments.
        options = ["", early, off] if (early or off) else []
        result = subprocess.run(
            ["just", "--justfile", str(JUSTFILE), "--working-directory", str(REPO), "android-repack-apk",
             str(self.apk), str(self.shim), os.path.relpath(work, REPO), ":".join(str(p) for p in plugins), "", "",
             str(self.patcher), *options],
            capture_output=True, text=True, env=self.env, timeout=300)
        self.signed = work / "r15_nevr-sentinel_signed.apk"
        return result

    def manifest_in_signed(self) -> dict:
        with zipfile.ZipFile(self.signed) as z:
            return json.loads(z.read("lib/arm64-v8a/libnevr_plugin_manifest.so"))

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
            self.assertEqual(z.read("lib/arm64-v8a/libr15.so"), self.libr15 + MARKER)
            self.assertEqual(z.read("lib/arm64-v8a/libovrplatformloader_orig.so")[:4], b"\x7fELF")

    def test_several_plugins_are_all_packaged(self):
        self.make_inputs(DEV_VERSION)
        first, second = self.plugin("libfirst.so"), self.plugin("libsecond.so", "int plugin_marker = 2;\n")
        result = self.repack_with(first, second)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify-signed OK", result.stdout)
        with zipfile.ZipFile(self.signed) as z:
            self.assertEqual(z.read("lib/arm64-v8a/libfirst.so"), first.read_bytes())
            self.assertEqual(z.read("lib/arm64-v8a/libsecond.so"), second.read_bytes())
            self.assertEqual(z.read("lib/arm64-v8a/libr15.so"), self.libr15 + MARKER)

    def test_a_plugin_path_with_a_space_is_packaged(self):
        self.make_inputs(DEV_VERSION)
        plugin = self.plugin("libspaced.so")
        spaced = self.tmp / "with space" / "libspaced.so"
        spaced.parent.mkdir()
        spaced.write_bytes(plugin.read_bytes())
        result = self.repack_with(spaced)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.signed) as z:
            self.assertEqual(z.read("lib/arm64-v8a/libspaced.so"), plugin.read_bytes())

    def test_every_plugin_is_checked_not_only_the_first(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libgood.so"), self.plugin("libother.so", ids=OTHER_ID))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build_id_unsupported", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_two_plugins_with_one_file_name_are_refused(self):
        self.make_inputs(DEV_VERSION)
        (self.tmp / "plugins" / "other").mkdir(parents=True)
        same = build_aarch64(self.tmp / "plugins" / "other" / "libgood.so", nevr_note(GAME_ID) + "int plugin_marker = 3;\n")
        result = self.repack_with(self.plugin("libgood.so"), same)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("two plugins are named libgood.so", result.stderr)
        self.assertFalse(self.signed.exists())

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

    def test_a_packaged_plugin_gets_a_manifest_entry_in_the_signed_apk(self):
        self.make_inputs(DEV_VERSION)
        plugin = self.plugin("libgood.so")
        result = self.repack_with(plugin)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.manifest_in_signed(), {
            "schema": 1,
            "plugins": [{"name": "good", "file": "libgood.so", "enabled": True, "early": False,
                         "sha256": hashlib.sha256(plugin.read_bytes()).hexdigest()}],
        })
        # The bytes themselves: sorted keys, no spaces, one trailing newline (the loader reads the text, not the parse).
        expected = ('{"plugins":[{"early":false,"enabled":true,"file":"libgood.so","name":"good","sha256":"%s"}],"schema":1}\n'
                    % hashlib.sha256(plugin.read_bytes()).hexdigest()).encode()
        with zipfile.ZipFile(self.signed) as z:
            self.assertEqual(z.read("lib/arm64-v8a/libnevr_plugin_manifest.so"), expected)

    def test_several_plugins_keep_their_argument_order_in_the_manifest(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libfirst.so"), self.plugin("libsecond.so", "int plugin_marker = 2;\n"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([p["name"] for p in self.manifest_in_signed()["plugins"]], ["first", "second"])

    def test_no_plugin_adds_no_manifest_entry(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.signed) as z:
            self.assertNotIn("lib/arm64-v8a/libnevr_plugin_manifest.so", z.namelist())

    def test_plugin_early_and_plugin_off_are_recorded(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libfirst.so"), self.plugin("libsecond.so", "int plugin_marker = 2;\n"),
                                  early="libfirst.so", off="libsecond.so")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        plugins = {p["name"]: p for p in self.manifest_in_signed()["plugins"]}
        self.assertTrue(plugins["first"]["early"])
        self.assertTrue(plugins["first"]["enabled"])
        self.assertFalse(plugins["second"]["early"])
        self.assertFalse(plugins["second"]["enabled"])

    def test_an_early_name_that_is_not_packaged_is_refused_before_anything_is_signed(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libgood.so"), early="libmissing.so")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libmissing.so", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_plugin_named_like_the_manifest_is_refused(self):
        self.make_inputs(DEV_VERSION)
        result = self.repack_with(self.plugin("libnevr_plugin_manifest.so"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("staged by the recipe", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_the_manifest_bytes_are_the_same_on_two_runs(self):
        self.make_inputs(DEV_VERSION)
        plugin = self.plugin("libgood.so")
        first = self.repack_with(plugin)
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        with zipfile.ZipFile(self.signed) as z:
            once = z.read("lib/arm64-v8a/libnevr_plugin_manifest.so")
        second = self.repack_with(plugin)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        with zipfile.ZipFile(self.signed) as z:
            self.assertEqual(z.read("lib/arm64-v8a/libnevr_plugin_manifest.so"), once)


@unittest.skipIf(MISSING, f"the recipe needs {', '.join(MISSING)}")
class RepackPatchesLibr15Test(RepackFixture):
    """`just android-repack-apk`: libr15.so goes through the patcher (the `patcher` parameter is the stand-in), is stored
    like the store's entry, and the patcher's file is checked again by verify-signed (ADR 0021 section 9)."""

    def run_recipe(self, *tail: str):
        work = self.tmp / "work"
        result = subprocess.run(
            ["just", "--justfile", str(JUSTFILE), "--working-directory", str(REPO), "android-repack-apk",
             str(self.apk), str(self.shim), os.path.relpath(work, REPO), *tail],
            capture_output=True, text=True, env=self.env, timeout=300)
        self.signed = work / "r15_nevr-sentinel_signed.apk"
        return result

    def test_the_signed_libr15_is_the_patchers_output_and_is_stored_deflated_like_the_store_entry(self):
        self.make_inputs(DEV_VERSION)
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.apk) as store, zipfile.ZipFile(self.signed) as out:
            self.assertEqual(store.getinfo("lib/arm64-v8a/libr15.so").compress_type, zipfile.ZIP_DEFLATED)
            self.assertEqual(out.getinfo("lib/arm64-v8a/libr15.so").compress_type, zipfile.ZIP_DEFLATED)
            self.assertEqual(out.read("lib/arm64-v8a/libr15.so"), PATCHED_LIBR15)
            # the two staged loaders are the stored, page-aligned entries
            self.assertEqual(out.getinfo("lib/arm64-v8a/libovrplatformloader.so").compress_type, zipfile.ZIP_STORED)
        self.assertIn("sha256 patched " + hashlib.sha256(PATCHED_LIBR15).hexdigest(), result.stdout)
        self.assertIn("sha256 signed  " + hashlib.sha256(PATCHED_LIBR15).hexdigest(), result.stdout)

    def test_the_patcher_runs_twice_with_no_extra_rows_when_none_are_given(self):
        self.make_inputs(DEV_VERSION)
        self.assertEqual(self.run_recipe("", "", "", str(self.patcher)).returncode, 0)
        runs = recorded_runs(self.patcher)
        self.assertEqual(len(runs), 2)  # the repack, then verify-signed on its own copy
        for argv in runs:
            self.assertNotIn("--extra-rows", argv)
        work = self.tmp / "work"
        repack, verify = (dict(zip(argv[0::2], argv[1::2])) for argv in runs)
        self.assertEqual(pathlib.Path(repack["--stock"]).resolve(), work.resolve() / "orig" / "lib" / "arm64-v8a" / "libr15.so")
        self.assertEqual(pathlib.Path(repack["--out"]).resolve(), work.resolve() / "stage" / "lib" / "arm64-v8a" / "libr15.so")
        self.assertEqual(pathlib.Path(repack["--report"]).resolve(), work.resolve() / "libr15-patch-report.json")
        self.assertTrue((work / "libr15-patch-report.json").is_file())
        self.assertEqual(pathlib.Path(verify["--stock"]).resolve().parent.parent, work.resolve())  # verify-signed's copy is under the work dir

    def test_the_extra_rows_file_goes_to_the_patcher_and_to_verify_signed(self):
        self.make_inputs(DEV_VERSION)
        rows = self.tmp / "rows.json"
        rows.write_text("{}")
        with self.shim.open("ab") as shim:  # the variant build embeds the file's digest (kSha256)
            shim.write(b"\0libr15_extra_rows\0" + hashlib.sha256(rows.read_bytes()).hexdigest().encode() + b"\0")
        result = self.run_recipe("", "", str(rows), str(self.patcher))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        runs = recorded_runs(self.patcher)
        self.assertEqual(len(runs), 2)
        for argv in runs:
            self.assertEqual(argv[argv.index("--extra-rows") + 1], str(rows))

    def test_a_shim_not_built_for_the_extra_rows_is_refused_before_anything_is_staged(self):
        self.make_inputs(DEV_VERSION)
        rows = self.tmp / "rows.json"
        rows.write_text("{}")  # make_inputs' shim carries no digest of it
        result = self.run_recipe("", "", str(rows), str(self.patcher))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("was not built with rows.json", result.stderr)
        self.assertEqual(recorded_runs(self.patcher), [])
        self.assertFalse(self.signed.exists())

    def test_a_variant_shim_without_an_extra_rows_file_is_refused_before_anything_is_staged(self):
        self.make_inputs(DEV_VERSION)
        with self.shim.open("ab") as shim:
            shim.write(b"\0libr15_extra_rows\0")
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is a variant build", result.stderr)
        self.assertEqual(recorded_runs(self.patcher), [])
        self.assertFalse(self.signed.exists())

    def test_an_extra_rows_file_that_is_not_there_is_refused_before_anything_is_staged(self):
        self.make_inputs(DEV_VERSION)
        result = self.run_recipe("", "", str(self.tmp / "no-rows.json"), str(self.patcher))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("extra rows file not found", result.stderr)
        self.assertEqual(recorded_runs(self.patcher), [])
        self.assertFalse(self.signed.exists())

    def test_a_patcher_that_refuses_stops_the_recipe_with_no_signed_apk(self):
        self.make_inputs(DEV_VERSION)
        self.patcher = make_standin(self.tmp / "patcher-fail", "fail")
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("standin patcher: refused", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_patcher_that_writes_nothing_stops_the_recipe_with_no_signed_apk(self):
        self.make_inputs(DEV_VERSION)
        self.patcher = make_standin(self.tmp / "patcher-nowrite", "nowrite")
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("the libr15 patcher wrote no libr15.so", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_patcher_path_that_is_not_an_executable_is_refused(self):
        self.make_inputs(DEV_VERSION)
        result = self.run_recipe("", "", "", str(self.tmp / "no-such-patcher"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("the libr15 patcher is not an executable file", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_a_json_include_without_the_header_is_refused_naming_it(self):
        self.make_inputs(DEV_VERSION)
        empty = self.tmp / "empty-include"
        empty.mkdir()
        result = self.run_recipe("", "", "", "", str(empty))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"android-repack-apk: FAIL - {empty}/nlohmann/json.hpp not found (json_include)", result.stderr)
        self.assertFalse(self.signed.exists())

    # ---- the reference runtime's libvrapi.so (#1737) ---------------------------------------------------------------------

    def add_libvrapi(self, data: bytes) -> None:
        """Adds lib/arm64-v8a/libvrapi.so to the fixture's store APK."""
        with zipfile.ZipFile(self.apk, "a", zipfile.ZIP_DEFLATED) as z:
            z.writestr(RUNTIME, data)

    def test_a_reference_runtime_is_pinned_to_the_staged_libr15_and_stored(self):
        self.make_inputs(DEV_VERSION)
        self.add_libvrapi(make_runtime())
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        pinned = make_runtime(hashlib.md5(PATCHED_LIBR15).hexdigest())
        with zipfile.ZipFile(self.signed) as out:
            self.assertEqual(out.read(RUNTIME), pinned)
            self.assertEqual(out.getinfo(RUNTIME).compress_type, zipfile.ZIP_STORED)
        self.assertIn('"event": "runtime_pinned"', result.stdout)
        self.assertIn(f"libvrapi.so sha256 signed  {hashlib.sha256(pinned).hexdigest()}", result.stdout)

    def test_a_libvrapi_with_no_checksum_list_is_left_as_the_store_has_it(self):
        self.make_inputs(DEV_VERSION)
        self.add_libvrapi(STORE_VRAPI)
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("carries no runtime md5 list: left as it is", result.stdout)
        with zipfile.ZipFile(self.signed) as out:
            self.assertEqual(out.read(RUNTIME), STORE_VRAPI)

    def test_a_reference_runtime_that_cannot_be_pinned_stops_the_recipe_with_no_signed_apk(self):
        self.make_inputs(DEV_VERSION)
        self.add_libvrapi(make_runtime(sites=11))
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("runtime_pin_refused", result.stderr)
        self.assertFalse(self.signed.exists())

    def test_an_apk_with_no_libvrapi_stages_none(self):
        self.make_inputs(DEV_VERSION)
        result = self.run_recipe("", "", "", str(self.patcher))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.signed) as out:
            self.assertNotIn(RUNTIME, out.namelist())



def aarch64_so(tag: bytes = b"") -> bytes:
    """The smallest 64-bit little-endian AArch64 ELF header the staging check reads, plus a tag that makes each file distinct."""
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2  # ELFCLASS64
    header[5] = 1  # ELFDATA2LSB
    struct.pack_into("<H", header, 18, 183)  # EM_AARCH64
    return bytes(header) + tag


class StageLibsTest(unittest.TestCase):
    """`stage-libs` (M5): the script libraries of a combat build are staged only when each is named, AArch64 and listed."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.store = self.root / "store.apk"
        with zipfile.ZipFile(self.store, "w") as zf:
            zf.writestr("lib/arm64-v8a/libr15.so", b"stock")
            zf.writestr("lib/arm64-v8a/libcollide0123.so", b"store")  # not a lib<hash16>.so name; never replaced here
        self.libs = self.root / "libs"
        self.libs.mkdir()
        self.stage = self.root / "stage"
        self.stage.mkdir()
        self.names = {"lib0123456789abcdef.so": aarch64_so(b"a"), "lib00000000000000ff.so": aarch64_so(b"b")}
        for name, data in self.names.items():
            (self.libs / name).write_bytes(data)
        self.write_sums()

    def write_sums(self, overrides=None):
        lines = []
        for name in sorted(self.names):
            data = (overrides or {}).get(name, self.names[name])
            lines.append(f"{SHA(data)}  {name}\n")
        (self.libs / "SHA256SUMS").write_text("".join(lines))

    def run_stage(self):
        return libs.stage_libs(self.libs, self.stage, self.store)

    def test_every_listed_library_is_staged_beside_the_plugins_and_no_replacement_is_recorded(self):
        self.assertEqual(self.run_stage(), [])
        for name, data in self.names.items():
            self.assertEqual((self.stage / "lib/arm64-v8a" / name).read_bytes(), data)
        self.assertEqual((self.stage / libs.STAGED_REPLACE_LIST).read_text(), "")
        self.assertEqual(libs.replaced_names(self.stage), [])

    def test_an_unlisted_library_is_refused(self):
        self.write_sums()
        (self.libs / "SHA256SUMS").write_text(f"{SHA(self.names['lib0123456789abcdef.so'])}  lib0123456789abcdef.so\n")
        problems = self.run_stage()
        self.assertTrue(any("lib00000000000000ff.so: not listed in SHA256SUMS" in p for p in problems), problems)
        self.assertFalse((self.stage / "lib/arm64-v8a").exists())

    def test_a_library_whose_bytes_differ_from_its_listed_digest_is_refused(self):
        self.write_sums(overrides={"lib0123456789abcdef.so": aarch64_so(b"not what was listed")})
        problems = self.run_stage()
        self.assertTrue(any("lib0123456789abcdef.so: its sha-256 differs" in p for p in problems), problems)

    def test_a_listed_name_with_no_file_is_refused(self):
        (self.libs / "lib00000000000000ff.so").unlink()
        problems = self.run_stage()
        self.assertTrue(any("lib00000000000000ff.so: listed in SHA256SUMS and missing" in p for p in problems), problems)

    def test_a_file_that_is_not_aarch64_is_refused(self):
        x86 = bytearray(aarch64_so(b"c"))
        struct.pack_into("<H", x86, 18, 62)  # EM_X86_64
        self.names["lib0123456789abcdef.so"] = bytes(x86)
        (self.libs / "lib0123456789abcdef.so").write_bytes(bytes(x86))
        self.write_sums()
        problems = self.run_stage()
        self.assertTrue(any("lib0123456789abcdef.so: not a 64-bit AArch64 shared object" in p for p in problems), problems)

    def test_a_stray_name_is_refused(self):
        for stray in ("libfoo.so", "lib0123456789abcdef.so.bak", "lib0123456789ABCDEF.so"):
            with self.subTest(name=stray):
                (self.libs / stray).write_bytes(aarch64_so(b"s"))
                problems = self.run_stage()
                self.assertTrue(any(f"{stray}: not named lib<hash16>.so" in p for p in problems), problems)
                (self.libs / stray).unlink()

    def test_a_name_the_store_holds_is_refused_unless_REPLACE_lists_it(self):
        name = "lib0123456789abcdef.so"
        with zipfile.ZipFile(self.store, "a") as zf:
            zf.writestr(f"lib/arm64-v8a/{name}", b"store copy")
        problems = self.run_stage()
        self.assertTrue(any(f"{name}: the store APK holds lib/arm64-v8a/{name}" in p for p in problems), problems)
        (self.libs / libs.LIBS_REPLACE).write_text(f"{name}\n")
        self.assertEqual(self.run_stage(), [])
        self.assertEqual(libs.replaced_names(self.stage), [name])

    def test_REPLACE_names_only_a_staged_library_that_replaces_a_store_library(self):
        (self.libs / libs.LIBS_REPLACE).write_text("lib0123456789abcdef.so\n")
        problems = self.run_stage()
        self.assertTrue(any("lib0123456789abcdef.so: REPLACE lists it" in p for p in problems), problems)


class StageRedirectedLibsTest(unittest.TestCase):
    """`stage-libs` (#1737): a redirected stock library is staged under its lib<hash16>_q15t.so name; its stock name never is."""

    HASH = "37f88adb0d8066f7"  # one of the nine libs.COMBAT_REDIRECT_HASHES
    STOCK = f"lib{HASH}.so"
    REDIRECTED = f"lib{HASH}_q15t.so"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.store = self.root / "store.apk"
        self.write_store(with_stock=True)
        self.libs = self.root / "libs"
        self.libs.mkdir()
        self.stage = self.root / "stage"
        self.stage.mkdir()
        self.data = aarch64_so(b"combat build")
        (self.libs / self.STOCK).write_bytes(self.data)
        self.write_sums()

    def write_store(self, *, with_stock):
        with zipfile.ZipFile(self.store, "w") as zf:
            zf.writestr("lib/arm64-v8a/libr15.so", b"stock")
            if with_stock:
                zf.writestr(f"lib/arm64-v8a/{self.STOCK}", b"stock script library")

    def write_sums(self):
        lines = [f"{SHA(p.read_bytes())}  {p.name}\n" for p in sorted(self.libs.iterdir()) if p.is_file() and p.name != "SHA256SUMS"]
        (self.libs / "SHA256SUMS").write_text("".join(lines))

    def test_the_redirected_library_is_staged_under_its_q15t_name_and_the_stock_name_is_not(self):
        self.assertEqual(libs.stage_libs(self.libs, self.stage, self.store), [])
        staged = self.stage / "lib/arm64-v8a"
        self.assertEqual((staged / self.REDIRECTED).read_bytes(), self.data)
        self.assertFalse((staged / self.STOCK).exists())
        self.assertEqual(libs.replaced_names(self.stage), [])

    def test_a_redirected_library_the_store_does_not_hold_is_refused(self):
        self.write_store(with_stock=False)
        problems = libs.stage_libs(self.libs, self.stage, self.store)
        self.assertTrue(any(f"{self.STOCK}: a redirected library, but the store APK holds no" in p for p in problems), problems)
        self.assertFalse((self.stage / "lib/arm64-v8a").exists())

    def test_a_redirected_library_named_in_REPLACE_is_refused(self):
        (self.libs / libs.LIBS_REPLACE).write_text(f"{self.STOCK}\n")
        problems = libs.stage_libs(self.libs, self.stage, self.store)
        self.assertTrue(any(f"{self.STOCK}: a redirected library never replaces" in p for p in problems), problems)
        self.assertFalse((self.stage / "lib/arm64-v8a").exists())

    def test_the_redirected_name_in_the_libraries_directory_is_refused(self):
        (self.libs / self.REDIRECTED).write_bytes(self.data)
        self.write_sums()
        problems = libs.stage_libs(self.libs, self.stage, self.store)
        self.assertTrue(any(f"{self.REDIRECTED}: not named lib<hash16>.so" in p for p in problems), problems)

    def test_the_redirect_mapping_covers_the_nine_and_no_other_name(self):
        self.assertEqual(len(set(libs.COMBAT_REDIRECT_HASHES)), 9)
        self.assertTrue(libs.is_redirected(self.STOCK))
        self.assertEqual(libs.staged_name(self.STOCK), self.REDIRECTED)
        self.assertFalse(libs.is_redirected("lib0123456789abcdef.so"))
        self.assertEqual(libs.staged_name("lib0123456789abcdef.so"), "lib0123456789abcdef.so")


SHA = lambda data: hashlib.sha256(data).hexdigest()  # noqa: E731


if __name__ == "__main__":
    unittest.main()
