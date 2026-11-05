"""src/quest/combat_libs: the combat script libraries are built by CMake on the game's NDK (r21b), pinned in tools/game_ndk.pin (#2047).

The libsrc directory is a small fixture made here (two tiny .cpp rows, one with a tiny .s, a fake LIBS.json): nothing from a
real libsrc and no game file is used. Two groups of tests:

- CMake-level: configure the project and check what it refuses. No compiler of the NDK is needed; every refusal below fires
  before or right after `project()`.
- Build: configure with the pinned NDK's own toolchain file, build the fixture, and check the outputs. This needs the NDK
  `just toolchain-r21b` installs; without it these tests are skipped and say so on stderr (a skip is never a pass).

The donor GOT tests (#2126) use a donor of two statics and two functions that load their addresses through the GOT. Assembled
and linked as it is, the pinned toolchain gives the two loads one GOT slot; the wrapper gen_donor_wrapper.cmake writes gives each
its own, and check_donor_got.cmake is the check on the linked library.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]
PROJECT = REPO / "src" / "quest" / "combat_libs"
INSTALLER = REPO / "tools" / "install_game_ndk.py"
H1 = "0123456789abcdef"
H2 = "fedcba9876543210"
PINNED_CLANG = re.compile(r"clang version 9\.0\.8")
LLVM = "toolchains/llvm/prebuilt/linux-x86_64/bin"


def squash(text: str) -> str:
    """CMake wraps long error lines; compare with the whitespace collapsed."""
    return re.sub(r"\s+", " ", text)


def link(asm=False, libr15=False, ldl=False) -> dict:
    return {"asm": asm, "libr15": libr15, "ldl": ldl}


class Fixture:
    """A libsrc directory, its parent's tools/scripts, a store APK and a build directory, all under one temporary root."""

    def __init__(self, root: pathlib.Path):
        self.root = root
        self.libsrc = root / "q" / "libsrc"
        self.scripts = root / "q" / "tools" / "scripts"
        self.build = root / "build"
        self.store = root / "store.apk"
        self.libsrc.mkdir(parents=True)
        self.scripts.mkdir(parents=True)
        (self.scripts / "regen_rt.h").write_text("#define REGEN_RT_FIXTURE 40\n")

    def write_libs(self, rows: dict[str, dict], names: dict[str, str] | None = None) -> None:
        libs = {}
        for h, lk in rows.items():
            key = (names or {}).get(h, f"lib{h}.so")
            libs[key] = {"md5": "0" * 32, "path": f"x/{key}", "link": lk}
            d = self.libsrc / h
            d.mkdir(exist_ok=True)
            (d / f"{h}.cpp").write_text(f"// {h}\nextern \"C\" int row_{h}(void) {{ return 1; }}\n")
            if lk["asm"]:
                (d / f"{h}.donor.s").write_text(".text\n")
        (self.libsrc / "LIBS.json").write_text(json.dumps({"libs": libs}))

    def write_store(self, members: dict[str, bytes]) -> None:
        with zipfile.ZipFile(self.store, "w") as zf:
            for name, data in members.items():
                zf.writestr(name, data)

    def configure_args(self, *extra: str, libsrc=True, store=True) -> list[str]:
        args = ["cmake", "-S", str(PROJECT), "-B", str(self.build), "-G", "Ninja"]
        if libsrc:
            args.append(f"-DNEVR_COMBAT_LIBSRC={self.libsrc}")
        if store:
            args.append(f"-DNEVR_STORE_APK={self.store}")
        return args + list(extra)


def run(args: list[str], **kw) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k not in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS")}
    return subprocess.run(args, capture_output=True, text=True, env=env, timeout=900, **kw)


TWO_STATICS = (".text\n.globl load_a\n.type load_a,%function\nload_a:\n\tadrp\tx0, :got:_dd_a\n\tldr\tx0, [x0, :got_lo12:_dd_a]\n\tret\n"
               ".globl load_b\n.type load_b,%function\nload_b:\n\tadrp\tx0, :got:_dd_b\n\tldr\tx0, [x0, :got_lo12:_dd_b]\n\tret\n"
               ".bss\n.align 3\n_dd_a:\t.zero 8\n_dd_b:\t.zero 8\n"
               # A data word that holds _dd_b's address: a relative relocation with _dd_b's address as its addend, outside .got.
               # It must not count as a GOT slot for _dd_b.
               ".data\n.globl ptr_b\nptr_b:\t.xword _dd_b\n")
GEN = PROJECT / "gen_donor_wrapper.cmake"
CHECK = PROJECT / "check_donor_got.cmake"


class DonorWrapperGenerator(unittest.TestCase):
    """gen_donor_wrapper.cmake: what it writes and what it refuses. It runs as `cmake -P`; no NDK is needed."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="donor-wrapper-")
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)
        self.donor = self.root / "d.donor.s"
        self.wrapper = self.root / "out" / "d.donor_got.s"

    def generate(self, donor_text: str) -> subprocess.CompletedProcess:
        self.donor.write_bytes(donor_text.encode())
        self.wrapper.parent.mkdir(exist_ok=True)
        return run(["cmake", f"-DNEVR_DONOR={self.donor}", f"-DNEVR_WRAPPER={self.wrapper}", "-P", str(GEN)])

    def test_the_wrapper_includes_the_donor_and_makes_each_got_operand_global_and_hidden_once(self):
        # CRLF, as the libsrc files are; a symbol named by two lines and by both halves of a load is declared once.
        done = self.generate(TWO_STATICS.replace("\n", "\r\n") + "\tadrp\tx1, :got:_dd_a\r\n")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.wrapper.read_text(), f'\t.include\t"{self.donor}"\n'
                         "\t.globl\t_dd_a\n\t.hidden\t_dd_a\n\t.globl\t_dd_b\n\t.hidden\t_dd_b\n")

    def test_a_donor_with_no_got_load_gets_a_wrapper_that_only_includes_it(self):
        done = self.generate(".text\nf:\n\tadrp\tx0, g\n\tadd\tx0, x0, :lo12:g\n\tret\n.bss\ng:\t.zero 8\n")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.wrapper.read_text(), f'\t.include\t"{self.donor}"\n')

    def test_an_operand_that_is_not_a_label_of_the_file_is_refused_and_no_wrapper_is_written(self):
        done = self.generate(TWO_STATICS.replace("_dd_b:\t.zero 8\n", ""))
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("_dd_b is a :got: operand but not a label defined in the file", squash(done.stderr))
        self.assertFalse(self.wrapper.exists())

    def test_an_operand_the_file_already_declares_global_is_refused(self):
        for directive in (".globl", ".global", ".weak"):
            with self.subTest(directive=directive):
                done = self.generate(TWO_STATICS + f"{directive} _dd_b\n")
                self.assertNotEqual(done.returncode, 0)
                self.assertIn("_dd_b is a :got: operand and is already declared global or weak", squash(done.stderr))
                self.assertFalse(self.wrapper.exists())

    def test_a_got_operand_that_is_not_a_plain_symbol_is_refused(self):
        for operand in ("_dd_a+8", "_dd_a@GOT", "(_dd_a)"):
            with self.subTest(operand=operand):
                done = self.generate(TWO_STATICS + f"\tadrp\tx2, :got:{operand}\n")
                self.assertNotEqual(done.returncode, 0)
                self.assertIn("a :got: operand this tool does not read", squash(done.stderr))
                self.assertFalse(self.wrapper.exists())

    def test_the_tool_names_its_missing_inputs(self):
        done = run(["cmake", "-P", str(GEN)])
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("pass -DNEVR_DONOR=<donor.s> and -DNEVR_WRAPPER=<out.s>", squash(done.stderr))


class CombatLibsConfigure(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="combat-libs-")
        self.addCleanup(self._tmp.cleanup)
        self.fx = Fixture(pathlib.Path(self._tmp.name))
        self.fx.write_libs({H1: link(libr15=True), H2: link(asm=True, ldl=True)})
        self.fx.write_store({"lib/arm64-v8a/libr15.so": b"stock libr15",
                             "lib/arm64-v8a/libovrplatformloader.so": b"loader"})

    def refused(self, needle: str, *extra: str, **kw) -> subprocess.CompletedProcess:
        result = run(self.fx.configure_args(*extra, **kw))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(needle, squash(result.stderr), result.stderr)
        self.assertIn("build-quest-combat-libs:", squash(result.stderr))
        self.assertFalse((self.fx.build / "build.ninja").exists(), "a refused configure writes no build")
        return result

    # --- the libsrc directory ---

    def test_no_libsrc_input_is_refused(self):
        self.refused("NEVR_COMBAT_LIBSRC names no libsrc directory", libsrc=False)

    def test_a_libsrc_that_is_not_a_directory_is_refused(self):
        self.fx.libsrc.rename(self.fx.root / "moved")
        self.refused("libsrc is not a directory")

    def test_a_missing_LIBS_json_is_refused(self):
        (self.fx.libsrc / "LIBS.json").unlink()
        self.refused(f"no LIBS.json in {self.fx.libsrc.resolve()}")

    def test_a_LIBS_json_that_does_not_parse_or_names_no_libs_is_refused(self):
        for text in ("{not json", "[]", '{"libs": {}}', '{"libs": []}', "{}"):
            with self.subTest(text=text):
                (self.fx.libsrc / "LIBS.json").write_text(text)
                self.refused("LIBS.json in")
                (self.fx.build / "CMakeCache.txt").unlink(missing_ok=True)

    def test_a_key_not_named_lib_hash16_so_is_refused(self):
        for bad in ("libfoo.so", f"lib{H1}.so.bak", "lib0123456789ABCDEF.so", f"lib{H1[:-1]}.so", f"lib{H1}0.so"):
            with self.subTest(name=bad):
                self.fx.write_libs({H1: link(libr15=True)}, names={H1: bad})
                self.refused(f"LIBS.json key '{bad}' is not named lib<hash16>.so")
                (self.fx.build / "CMakeCache.txt").unlink(missing_ok=True)

    def test_a_row_without_a_link_object_of_booleans_is_refused(self):
        for bad in (None, "yes", {"asm": 1, "libr15": False, "ldl": False}, {"asm": False, "libr15": "no"}):
            with self.subTest(link=bad):
                self.fx.write_libs({H1: link(libr15=True)})
                data = json.loads((self.fx.libsrc / "LIBS.json").read_text())
                if bad is None:
                    del data["libs"][f"lib{H1}.so"]["link"]
                else:
                    data["libs"][f"lib{H1}.so"]["link"] = bad
                (self.fx.libsrc / "LIBS.json").write_text(json.dumps(data))
                self.refused(f'LIBS.json row lib{H1}.so has no "link" object of booleans asm, libr15 and ldl')
                (self.fx.build / "CMakeCache.txt").unlink(missing_ok=True)

    def test_a_link_key_that_is_absent_is_false_and_is_not_refused(self):
        (self.fx.libsrc / "LIBS.json").write_text(json.dumps({"libs": {f"lib{H1}.so": {"link": {"asm": False}}}}))
        result = run(self.fx.configure_args())
        self.assertIn("no CMAKE_TOOLCHAIN_FILE", squash(result.stderr), "the row is accepted; the first refusal is the toolchain")
        self.assertNotIn('has no "link" object', squash(result.stderr))

    def test_a_row_that_is_not_an_object_is_refused(self):
        (self.fx.libsrc / "LIBS.json").write_text(json.dumps({"libs": {f"lib{H1}.so": 3}}))
        self.refused(f'LIBS.json row lib{H1}.so has no "link" object')

    def test_a_missing_source_is_refused(self):
        (self.fx.libsrc / H2 / f"{H2}.cpp").unlink()
        self.refused(f"source of lib{H2}.so is missing: {self.fx.libsrc.resolve() / H2 / (H2 + '.cpp')}")

    def test_a_row_that_links_its_donor_object_needs_the_donor(self):
        (self.fx.libsrc / H2 / f"{H2}.donor.s").unlink()
        self.refused(f"lib{H2}.so links its donor object, and {self.fx.libsrc.resolve() / H2 / (H2 + '.donor.s')} is missing")

    def test_the_libsrc_is_checked_before_the_store_apk(self):
        (self.fx.libsrc / "LIBS.json").unlink()
        result = self.refused("no LIBS.json in", store=False)
        self.assertNotIn("NEVR_STORE_APK", squash(result.stderr))

    # --- the store APK ---

    def test_no_store_apk_input_is_refused(self):
        self.refused("NEVR_STORE_APK names no store APK", store=False)

    def test_a_store_apk_that_does_not_exist_is_refused(self):
        self.fx.store.unlink()
        self.refused(f"store APK not found: {self.fx.store}")

    def test_a_store_apk_that_is_not_a_zip_is_refused(self):
        self.fx.store.write_bytes(b"this is not a zip file at all")
        self.refused(f"the store APK is not a zip file: {self.fx.store}")

    def test_a_store_apk_without_libr15_is_refused(self):
        self.fx.write_store({"lib/arm64-v8a/libovrplatformloader.so": b"loader"})
        self.refused("the store APK has no lib/arm64-v8a/libr15.so")

    # --- the toolchain ---

    def test_a_configure_without_a_toolchain_file_is_refused(self):
        self.refused("no CMAKE_TOOLCHAIN_FILE")

    def test_a_compiler_that_is_not_the_pinned_clang_is_refused_naming_both_lines(self):
        empty = self.fx.root / "empty-toolchain.cmake"
        empty.write_text("# the host compilers\n")
        result = self.refused("is not the game's NDK clang pinned in tools/game_ndk.pin", f"-DCMAKE_TOOLCHAIN_FILE={empty}")
        self.assertIn("expected: Android (6317467 based on r365631c1) clang version 9.0.8", squash(result.stderr))
        self.assertIn("just toolchain-r21b", squash(result.stderr))

    # --- the preset ---

    def test_the_preset_names_the_toolchain_file_of_the_game_ndk_from_the_environment(self):
        presets = json.loads((PROJECT / "CMakePresets.json").read_text())
        configure = {p["name"]: p for p in presets["configurePresets"]}["quest-combat-libs"]
        self.assertEqual(configure["generator"], "Ninja")
        self.assertEqual(configure["toolchainFile"], "$env{NEVR_GAME_NDK}/build/cmake/android.toolchain.cmake")
        self.assertEqual(configure["binaryDir"], "${sourceDir}/../../../build/quest-combat-libs")
        self.assertEqual((PROJECT / "../../../build/quest-combat-libs").resolve(), (REPO / "build" / "quest-combat-libs").resolve())
        builds = {p["name"]: p for p in presets["buildPresets"]}
        self.assertEqual(builds["quest-combat-libs"]["configurePreset"], "quest-combat-libs")

    # --- the recipe ---

    def test_the_recipe_runs_cmake_not_the_python_builder(self):
        text = (REPO / "justfile").read_text(encoding="utf-8")
        start = text.index("\nbuild-quest-combat-libs ")
        recipe = text[start:text.index("\n\n#", start)]
        self.assertIn('libsrc=env("NEVR_COMBAT_LIBSRC", "")', recipe.splitlines()[1])
        self.assertIn("NEVR_COMBAT_LIBSRC names no libsrc directory", recipe)
        self.assertIn("NEVR_STORE_APK names no store APK", recipe)
        self.assertIn("src/quest/combat_libs", recipe)
        # ADR 0041 rule 2: the NDK's toolchain file is named by a preset, never passed to cmake by the recipe (violation V2).
        self.assertIn("cmake --preset quest-combat-libs", recipe)
        self.assertIn("cmake --build --preset quest-combat-libs", recipe)
        self.assertIn("tools/install_game_ndk.py --print-dir", recipe)
        self.assertRegex(recipe, r"NEVR_GAME_NDK=\$\(python3 -I tools/install_game_ndk\.py --print-dir\)")
        self.assertIn("export NEVR_GAME_NDK", recipe)
        self.assertIn("build/quest-combat-libs", recipe)
        for forbidden in ("-DCMAKE_TOOLCHAIN_FILE", "cmake -S", " -S ", " -B ", "-G Ninja", "clang"):
            self.assertNotIn(forbidden, recipe)
        self.assertNotIn("build_quest_combat_libs.py", recipe)
        self.assertNotIn("ANDROID_NDK_HOME", recipe)
        self.assertFalse((REPO / "tools" / "build_quest_combat_libs.py").exists())

    def test_the_recipe_writes_the_directory_the_repack_takes(self):
        # #2092: android-repack-apk's stage-libs refuses the CMake build tree (CMakeFiles/, build.ninja, stock/ ...); the recipe
        # gives it a directory of the libraries and SHA256SUMS only, and checks the sums there.
        text = (REPO / "justfile").read_text(encoding="utf-8")
        start = text.index("\nbuild-quest-combat-libs ")
        recipe = text[start:text.index("\n\n#", start)]
        self.assertIn('libs="$out/libs"', recipe)
        self.assertIn('done < "$out/SHA256SUMS"', recipe)  # the names SHA256SUMS lists, never a glob over the build tree
        self.assertIn("no library was built", recipe)
        self.assertIn('(cd "$libs" && sha256sum -c --quiet SHA256SUMS)', recipe)
        self.assertIn("the repack takes build/quest-combat-libs/libs", recipe)


def pinned_ndk() -> pathlib.Path | None:
    done = subprocess.run([sys.executable, "-I", str(INSTALLER), "--print-dir"], capture_output=True, text=True)
    path = pathlib.Path(done.stdout.strip()) if done.returncode == 0 else None
    return path if path and (path / LLVM / "clang").is_file() else None


class CombatLibsBuild(unittest.TestCase):
    """Build the fixture with the pinned NDK. Skipped, with the reason on stderr, when `just toolchain-r21b` has not run."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.ndk = pinned_ndk()
        cls.reason = None
        if cls.ndk is None:
            cls.reason = "SKIPPED: the pinned r21b NDK is not installed (run `just toolchain-r21b`); the fixture build did not run"
            print(cls.reason, file=sys.stderr)

    def setUp(self) -> None:
        if self.reason:
            self.skipTest(self.reason)
        self._tmp = tempfile.TemporaryDirectory(prefix="combat-libs-ndk-")
        self.addCleanup(self._tmp.cleanup)
        self.fx = Fixture(pathlib.Path(self._tmp.name))
        self.llvm = self.ndk / LLVM

    def make_stock(self) -> None:
        """A stock libr15.so stand-in built with the NDK: it defines libr15_fixture and, like the store's file, has no SONAME."""
        src = self.fx.root / "stock.cpp"
        src.write_text('extern "C" int libr15_fixture(void) { return 2; }\n')
        so = self.fx.root / "libr15.so"
        done = run([str(self.llvm / "aarch64-linux-android21-clang++"), "-shared", "-fPIC", "-nostdlib++",
                    str(src), "-o", str(so)])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.fx.write_store({"lib/arm64-v8a/libr15.so": so.read_bytes()})

    def write_rows(self) -> None:
        self.fx.write_libs({H1: link(libr15=True), H2: link(asm=True, ldl=True)})
        (self.fx.libsrc / H1 / f"{H1}.cpp").write_text(
            '#include "regen_rt.h"\n#include <cstdint>\n#include <string.h>\nextern "C" int libr15_fixture(void);\n'
            'extern "C" void sink(char *);\nextern "C" int _Unwind_Backtrace(void *, void *);\n'
            # An array on the stack that a call fills with an unknown length: -fstack-protector-strong guards it, and
            # _FORTIFY_SOURCE=2 would turn the memcpy into __memcpy_chk.
            'extern "C" void row_copy(const char *src, unsigned n) { char buf[8]; memcpy(buf, src, n); sink(buf); }\n'
            # _Unwind_Backtrace pulls the NDK's static libgcc unwinder in, whose objects carry .debug_* sections of their own.
            f'extern "C" int row_{H1}(void) {{ std::int32_t v = libr15_fixture() + REGEN_RT_FIXTURE; return v + _Unwind_Backtrace(0, 0); }}\n')
        (self.fx.libsrc / H2 / f"{H2}.cpp").write_text(
            '#include <dlfcn.h>\nextern "C" int donor(void);\nextern "C" int resolved_at_load(void);\n'
            f'extern "C" int row_{H2}(void) {{ return dlopen("x", RTLD_NOW) ? donor() + resolved_at_load() : 0; }}\n')
        (self.fx.libsrc / H2 / f"{H2}.donor.s").write_text(
            ".text\n.globl donor\n.type donor,%function\ndonor:\n  mov w0, #7\n  ret\n")

    def configure(self, *extra: str) -> subprocess.CompletedProcess:
        tc = self.ndk / "build" / "cmake" / "android.toolchain.cmake"
        return run(self.fx.configure_args(f"-DCMAKE_TOOLCHAIN_FILE={tc}", *extra))

    def build(self) -> subprocess.CompletedProcess:
        return run(["cmake", "--build", str(self.fx.build)])

    def readelf(self, *args: str) -> str:
        done = run([str(self.llvm / "llvm-readelf"), *args])
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout

    def test_the_fixture_builds_with_the_flags_each_row_names_and_writes_lib_hash16_so_and_SHA256SUMS(self):
        self.make_stock()
        self.write_rows()
        conf = self.configure()
        self.assertEqual(conf.returncode, 0, conf.stdout + conf.stderr)
        built = self.build()
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        out = self.fx.build
        for h in (H1, H2):
            self.assertTrue((out / f"lib{h}.so").is_file(), f"lib{h}.so is in the build directory")
        # SHA256SUMS: "<sha256>  <name>" per library, sorted by name, over the outputs.
        want = sorted(f"{hashlib.sha256((out / f'lib{h}.so').read_bytes()).hexdigest()}  lib{h}.so" for h in (H1, H2))
        want.sort(key=lambda line: line.split()[1])
        self.assertEqual((out / "SHA256SUMS").read_text().splitlines(), want)
        # The stock libr15.so is the one in the store APK, extracted next to the build.
        self.assertEqual((out / "stock" / "libr15.so").read_bytes(), zipfile.ZipFile(self.fx.store).read("lib/arm64-v8a/libr15.so"))
        # Row 1 links the stock library, named by file name (NEEDED libr15.so, not a path); row 2 does not.
        dyn1 = self.readelf("-d", "-W", str(out / f"lib{H1}.so"))
        dyn2 = self.readelf("-d", "-W", str(out / f"lib{H2}.so"))
        self.assertIn("Shared library: [libr15.so]", dyn1)
        self.assertNotIn("libr15", dyn2)
        # Row 2 adds -ldl on its link line and row 1 does not (the driver's own NEEDED libdl.so is on both).
        link1 = run(["ninja", "-C", str(out), "-t", "commands", f"combat_lib_{H1}"]).stdout.splitlines()[-1]
        link2 = run(["ninja", "-C", str(out), "-t", "commands", f"combat_lib_{H2}"]).stdout.splitlines()[-1]
        self.assertNotIn("-ldl", link1.split())
        self.assertIn("-ldl", link2.split())
        for flag in ("-nostdlib++", "-Wl,--gc-sections", "-Wl,--allow-shlib-undefined"):
            self.assertIn(flag, link1.split())
        # Neither links the C++ runtime (-nostdlib++), and neither has NEEDED libm.so: the store's script libraries have libc and
        # libdl only. The link is done by the C driver, which does not add -lm.
        self.assertNotIn("libc++", dyn1 + dyn2)
        for dyn in (dyn1, dyn2):
            self.assertIn("Shared library: [libc.so]", dyn)
            self.assertIn("Shared library: [libdl.so]", dyn)
            self.assertNotIn("libm.so", dyn)
        driver = link1.split()[link1.split().index("&&") + 1]
        self.assertTrue(driver.endswith("/clang"), driver)
        # What the store's libraries show, and the NDK toolchain file's defaults would not give: no debug section, no build-id
        # note (the donor object of row 2 has none either), no fortified libc import, no frame pointer set up.
        for lib in (H1, H2):
            sections = self.readelf("-S", "-W", str(out / f"lib{lib}.so"))
            self.assertNotIn(".debug_", sections)
            self.assertNotIn(".note.gnu.build-id", sections)
        und1 = run([str(self.llvm / "llvm-nm"), "-D", "--undefined-only", str(out / f"lib{H1}.so")]).stdout
        self.assertNotIn("__memcpy_chk", und1)
        self.assertIn("__stack_chk_fail", und1, "the stack protector stays: every store script library imports __stack_chk_fail")
        asm1 = run([str(self.llvm / "llvm-objdump"), "-d", "--no-show-raw-insn", str(out / f"lib{H1}.so")]).stdout
        # row_copy is not a leaf (it calls memcpy and sink), which the NDK's default compiles with a frame pointer.
        body = re.search(r"\brow_copy:\n(.*?)(?:\n\n|\Z)", asm1, re.S)
        self.assertIsNotNone(body, "row_copy is in the library")
        self.assertIn("bl", body.group(1))
        self.assertNotRegex(body.group(1), r"\b(?:add|mov)\s+x29, sp\b")
        self.assertIn("-Wl,--build-id=none", link1.split())
        self.assertIn("-Wl,--strip-debug", link1.split())
        for kept in ("-latomic", "-Wl,--fatal-warnings", "-fstack-protector-strong"):
            self.assertIn(kept, link1.split(), "the toolchain file's flag the store libraries cannot settle is kept")
        self.assertIn("dl_iterate_phdr", und1, "the unwinder of libgcc.a is linked in, so the strip is exercised")
        # The donor object is part of row 2: its symbol is defined in the library, and row 1 does not define it.
        defined2 = run([str(self.llvm / "llvm-nm"), "--defined-only", str(out / f"lib{H2}.so")]).stdout
        self.assertRegex(defined2, r"\bT donor\b")
        # Built by the pinned NDK: the ident note names r21b / 6352462 and the comment names clang 9.0.8.
        ident = self.readelf("-p", ".note.android.ident", str(out / f"lib{H1}.so"))
        self.assertRegex(ident, r"\br21b\b")
        self.assertIn("6352462", ident)
        comment = run([str(self.llvm / "llvm-readelf"), "-p", ".comment", str(out / f"lib{H1}.so")]).stdout
        self.assertRegex(comment, PINNED_CLANG)
        # The flags of the compile: -O2 -fPIC, the include directory beside the libsrc's parent.
        commands = json.loads(run(["ninja", "-C", str(out), "-t", "compdb"]).stdout)
        cmd1 = next(c["command"] for c in commands if c["file"].endswith(f"{H1}.cpp"))
        self.assertIn("-O2", cmd1)
        self.assertIn("-fPIC", cmd1)
        self.assertIn("--target=aarch64-none-linux-android21", cmd1)
        for flag in ("-g0", "-U_FORTIFY_SOURCE", "-fomit-frame-pointer"):
            self.assertIn(flag, cmd1.split())
        for kept in ("-fstack-protector-strong", "-funwind-tables", "-fdata-sections", "-ffunction-sections"):
            self.assertIn(kept, cmd1.split(), "the toolchain file's flag the store libraries show or cannot settle is kept")
        obj1 = next(c["output"] for c in commands if c["file"].endswith(f"{H1}.cpp"))
        objsec = self.readelf("-S", "-W", str(out / obj1))
        self.assertNotIn(".debug_", objsec, "-g0 leaves no debug section in the compiled row")
        obj2 = next(c["output"] for c in commands if c["file"].endswith(f"{H2}.donor_got.s") and c["output"].endswith(".o"))
        self.assertNotIn(".debug_", self.readelf("-S", "-W", str(out / obj2)), "nor in the assembled donor object")
        self.assertGreater(cmd1.split().index("-U_FORTIFY_SOURCE"), cmd1.split().index("-D_FORTIFY_SOURCE=2"))
        self.assertGreater(cmd1.split().index("-g0"), cmd1.split().index("-g"))
        self.assertIn(f"-I{self.fx.scripts.resolve()}", cmd1)

    def test_the_sums_script_sorts_by_name_and_refuses_a_library_that_was_not_built(self):
        script = PROJECT / "write_sha256sums.cmake"
        out = self.fx.root / "sums"
        out.mkdir()
        for name, data in ((f"lib{H2}.so", b"two"), (f"lib{H1}.so", b"one")):
            (out / name).write_bytes(data)
        names = out / "names.txt"
        names.write_text(f"lib{H2}.so\nlib{H1}.so\n")
        done = run(["cmake", f"-DNEVR_NAMES_FILE={names}", f"-DNEVR_OUT_DIR={out}", "-P", str(script)])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual((out / "SHA256SUMS").read_text(), (
            f"{hashlib.sha256(b'one').hexdigest()}  lib{H1}.so\n{hashlib.sha256(b'two').hexdigest()}  lib{H2}.so\n"))
        (out / f"lib{H1}.so").unlink()
        gone = run(["cmake", f"-DNEVR_NAMES_FILE={names}", f"-DNEVR_OUT_DIR={out}", "-P", str(script)])
        self.assertNotEqual(gone.returncode, 0)
        self.assertIn(f"lib{H1}.so was not built", squash(gone.stderr))

    def test_the_preset_configures_and_builds_the_fixture_on_the_ndk_the_environment_names(self):
        self.make_stock()
        self.write_rows()
        env = {**os.environ, "NEVR_GAME_NDK": str(self.ndk)}
        binary = self.fx.build
        conf = subprocess.run(["cmake", "--preset", "quest-combat-libs", "-B", str(binary), f"-DNEVR_COMBAT_LIBSRC={self.fx.libsrc}",
                               f"-DNEVR_STORE_APK={self.fx.store}"], cwd=PROJECT, capture_output=True, text=True, env=env, timeout=900)
        self.assertEqual(conf.returncode, 0, conf.stdout + conf.stderr)
        cache = (binary / "CMakeCache.txt").read_text()
        self.assertIn(f"CMAKE_TOOLCHAIN_FILE:FILEPATH={self.ndk}/build/cmake/android.toolchain.cmake", cache)
        self.assertNotIn("Compatibility with CMake < 3.10", conf.stdout + conf.stderr, "the preset silences the NDK file's deprecation")
        built = self.build()  # the fixture's directory, not the preset's own build/quest-combat-libs
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        self.assertTrue((binary / f"lib{H1}.so").is_file())

    def test_a_preset_configure_without_the_ndk_in_the_environment_fails_on_the_toolchain_file(self):
        self.make_stock()
        self.write_rows()
        env = {k: v for k, v in os.environ.items() if k != "NEVR_GAME_NDK"}
        conf = subprocess.run(["cmake", "--preset", "quest-combat-libs", "-B", str(self.fx.build),
                               f"-DNEVR_COMBAT_LIBSRC={self.fx.libsrc}", f"-DNEVR_STORE_APK={self.fx.store}"],
                              cwd=PROJECT, capture_output=True, text=True, env=env, timeout=300)
        self.assertNotEqual(conf.returncode, 0)
        self.assertFalse((self.fx.build / "build.ninja").exists())

    def test_a_c_compiler_that_is_not_the_pinned_clang_is_refused_though_the_cxx_compiler_is(self):
        # The rows are linked by the C driver, so its version is gated as well as the C++ compiler's.
        stub = self.fx.root / "not-clang.sh"
        stub.write_text("#!/bin/sh\necho 'some other compiler 1.0'\n")
        stub.chmod(0o755)
        wrapper = self.fx.root / "wrapper-toolchain.cmake"
        tc = self.ndk / "build" / "cmake" / "android.toolchain.cmake"
        wrapper.write_text(f'include("{tc}")\nset(CMAKE_C_COMPILER "{stub}")\n')
        self.make_stock()
        self.write_rows()
        result = run(self.fx.configure_args(f"-DCMAKE_TOOLCHAIN_FILE={wrapper}"))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"the compiler {stub} is not the game's NDK clang", squash(result.stderr))
        self.assertIn("actual: some other compiler 1.0", squash(result.stderr))

    def test_an_asm_compiler_that_is_not_the_pinned_clang_is_refused_when_a_row_has_a_donor_object(self):
        # 108 of the 116 real rows assemble a donor .s, so the assembler is gated as well. The stand-in forwards everything to
        # the real clang except --version, so CMake's own checks of it pass and only the pin comparison can refuse it.
        stub = self.fx.root / "asm-not-clang.sh"
        stub.write_text(f"#!/bin/sh\nif [ \"$1\" = --version ]; then echo 'some other assembler 2.0'; exit 0; fi\n"
                        f"exec {self.llvm / 'clang'} \"$@\"\n")
        stub.chmod(0o755)
        wrapper = self.fx.root / "wrapper-asm-toolchain.cmake"
        tc = self.ndk / "build" / "cmake" / "android.toolchain.cmake"
        wrapper.write_text(f'include("{tc}")\nset(CMAKE_ASM_COMPILER "{stub}")\n')
        self.make_stock()
        self.write_rows()
        result = run(self.fx.configure_args(f"-DCMAKE_TOOLCHAIN_FILE={wrapper}"))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"the compiler {stub} is not the game's NDK clang", squash(result.stderr))
        self.assertIn("actual: some other assembler 2.0", squash(result.stderr))

    def test_a_library_with_a_dynamic_initializer_fails_the_static_init_check(self):
        self.make_stock()
        self.write_rows()
        (self.fx.libsrc / H1 / f"{H1}.cpp").write_text(
            'extern "C" int libr15_fixture(void);\nstruct Init { Init() { libr15_fixture(); } };\nstatic Init g_init;\n')
        self.assertEqual(self.configure().returncode, 0)
        built = self.build()
        self.assertNotEqual(built.returncode, 0, built.stdout)
        text = built.stdout + built.stderr
        self.assertIn("check_quest_static_init", text)
        self.assertIn(f"lib{H1}.so", text)
        self.assertFalse((self.fx.build / "SHA256SUMS").exists(), "no sums over a failed build")

    def test_a_source_that_does_not_compile_names_the_library(self):
        self.make_stock()
        self.write_rows()
        (self.fx.libsrc / H2 / f"{H2}.cpp").write_text("this is not C++\n")
        self.assertEqual(self.configure().returncode, 0)
        built = self.build()
        self.assertNotEqual(built.returncode, 0)
        self.assertIn(f"{H2}.cpp", built.stdout + built.stderr)

    # --- the donor GOT (#2126) ---

    def clang(self, *args: str) -> subprocess.CompletedProcess:
        return run([str(self.llvm / "aarch64-linux-android21-clang"), *args])

    def got_slots(self, lib: pathlib.Path) -> tuple[dict[int, int], dict[str, int]]:
        """The R_AARCH64_RELATIVE slots of .got as {slot address: content}, and the library's symbols as {name: address}."""
        sections = self.readelf("-S", "-W", str(lib))
        got = re.search(r"\]\s+\.got\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s", sections)
        self.assertIsNotNone(got, sections)
        begin = int(got.group(1), 16)
        end = begin + int(got.group(2), 16)
        slots = {}
        for line in self.readelf("-r", "-W", str(lib)).splitlines():
            m = re.match(r"^([0-9a-f]+)\s+[0-9a-f]+\s+R_AARCH64_RELATIVE\s+([0-9a-f]+)$", line.strip())
            if m and begin <= int(m.group(1), 16) < end:
                slots[int(m.group(1), 16)] = int(m.group(2), 16)
        nm = run([str(self.llvm / "llvm-nm"), str(lib)]).stdout
        symbols = {m.group(2): int(m.group(1), 16) for m in re.finditer(r"^([0-9a-f]+) [a-zA-Z] (\S+)$", nm, re.M)}
        return slots, symbols

    def load_slot(self, lib: pathlib.Path, function: str) -> int:
        """The address of the GOT slot the `adrp; ldr` pair at the start of `function` reads."""
        dis = run([str(self.llvm / "llvm-objdump"), "-d", "--no-show-raw-insn", str(lib)]).stdout
        body = re.search(rf"\b{function}:\n(.*?)(?:\n\n|\Z)", dis, re.S)
        self.assertIsNotNone(body, f"{function} is in the library")
        pair = re.search(r"([0-9a-f]+):\s+adrp\s+x0, #(-?\d+)\n\s+[0-9a-f]+:\s+ldr\s+x0, \[x0, #(\d+)\]", body.group(1))
        self.assertIsNotNone(pair, body.group(1))
        return (int(pair.group(1), 16) & ~0xFFF) + int(pair.group(2)) + int(pair.group(3))

    def build_two_statics(self) -> pathlib.Path:
        """The project build of one row whose donor object is TWO_STATICS; the library is returned."""
        self.fx.write_libs({H2: link(asm=True)})
        (self.fx.libsrc / H2 / f"{H2}.donor.s").write_bytes(TWO_STATICS.encode())
        self.fx.write_store({"lib/arm64-v8a/libr15.so": b"unused: no row links it"})
        conf = self.configure()
        self.assertEqual(conf.returncode, 0, conf.stdout + conf.stderr)
        built = self.build()
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        self.built_output = built.stdout + built.stderr
        return self.fx.build / f"lib{H2}.so"

    def unwrapped_two_statics(self) -> pathlib.Path:
        """The same donor assembled and linked as it was before the wrapper: from the donor.s alone."""
        src = self.fx.root / "raw.s"
        src.write_bytes(TWO_STATICS.encode())
        obj = self.fx.root / "raw.o"
        lib = self.fx.root / "raw.so"
        done = self.clang("-fPIC", "-O2", "-g0", "-c", str(src), "-o", str(obj))
        self.assertEqual(done.returncode, 0, done.stderr)
        done = self.clang("-shared", "-nostdlib", "-Wl,--build-id=none", str(obj), "-o", str(lib))
        self.assertEqual(done.returncode, 0, done.stderr)
        return lib

    def test_two_statics_loaded_through_the_got_share_one_slot_when_the_donor_is_assembled_alone(self):
        # The fault #2126 names: the assembler relocates both loads against .bss plus an offset, and the linker makes one slot
        # for the section and drops the offset. This test pins the toolchain's behaviour; it fails if the pin ever stops having it.
        lib = self.unwrapped_two_statics()
        slots, symbols = self.got_slots(lib)
        self.assertEqual(len(slots), 1, slots)
        self.assertEqual(self.load_slot(lib, "load_a"), self.load_slot(lib, "load_b"))
        self.assertEqual(list(slots.values()), [symbols["_dd_a"]], "the one slot holds _dd_a: load_b gets load_a's address")

    def test_two_statics_loaded_through_the_got_get_a_slot_each_and_neither_is_exported_with_the_wrapper(self):
        lib = self.build_two_statics()
        slots, symbols = self.got_slots(lib)
        self.assertEqual(slots.get(self.load_slot(lib, "load_a")), symbols["_dd_a"])
        self.assertEqual(slots.get(self.load_slot(lib, "load_b")), symbols["_dd_b"])
        self.assertNotEqual(self.load_slot(lib, "load_a"), self.load_slot(lib, "load_b"))
        exported = run([str(self.llvm / "llvm-nm"), "-D", "--defined-only", str(lib)]).stdout
        self.assertNotIn("_dd_", exported, "the wrapper's .hidden keeps the donor's statics out of the dynamic symbol table")
        self.assertRegex(exported, r"\bT load_a\b", "what the donor exports stays exported")
        self.assertIn("donor GOT check: ", self.built_output)
        self.assertIn(f"check_donor_got: OK lib{H2}.so: 2 of 2 symbols", self.built_output)

    def check(self, lib: pathlib.Path, donor: pathlib.Path) -> subprocess.CompletedProcess:
        return run(["cmake", f"-DNEVR_LLVM_BIN={self.llvm}", f"-DNEVR_LIB={lib}", f"-DNEVR_DONOR={donor}", "-P", str(CHECK)])

    def test_the_check_passes_the_wrapped_library_and_refuses_the_one_linked_from_the_donor_alone(self):
        wrapped = self.build_two_statics()
        donor = self.fx.libsrc / H2 / f"{H2}.donor.s"
        ok = self.check(wrapped, donor)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertIn(f"OK lib{H2}.so: 2 of 2 symbols", ok.stdout + ok.stderr)
        raw = self.unwrapped_two_statics()
        bad = self.check(raw, self.fx.root / "raw.s")
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("raw.so: 1 of 2 symbols the donor loads through the GOT have no GOT slot that holds their address, first: "
                      "_dd_b (at 0x", squash(bad.stderr))

    def test_the_check_refuses_a_symbol_the_library_does_not_define_and_a_library_whose_got_holds_none_of_the_donors(self):
        wrapped = self.build_two_statics()
        other = self.fx.root / "other.s"
        other.write_bytes(TWO_STATICS.replace("_dd_b", "_dd_missing").encode())
        gone = self.check(wrapped, other)
        self.assertNotEqual(gone.returncode, 0)
        self.assertIn("_dd_missing (not in the symbol table)", squash(gone.stderr))
        flat = self.fx.root / "flat.s"
        flat.write_bytes(b".text\n.globl flat\nflat:\n\tret\n.bss\n_dd_a:\t.zero 8\n_dd_b:\t.zero 8\n")
        flat_o = self.fx.root / "flat.o"
        flat_so = self.fx.root / "flat.so"
        self.assertEqual(self.clang("-fPIC", "-c", str(flat), "-o", str(flat_o)).returncode, 0)
        self.assertEqual(self.clang("-shared", "-nostdlib", str(flat_o), "-o", str(flat_so)).returncode, 0)
        empty = self.check(flat_so, self.fx.libsrc / H2 / f"{H2}.donor.s")
        self.assertNotEqual(empty.returncode, 0)
        self.assertIn("flat.so: 2 of 2 symbols the donor loads through the GOT have no GOT slot", squash(empty.stderr))

    def test_a_donor_that_names_an_undefined_got_label_stops_the_build_and_names_it(self):
        self.fx.write_libs({H2: link(asm=True)})
        (self.fx.libsrc / H2 / f"{H2}.donor.s").write_bytes(TWO_STATICS.replace("_dd_b:\t.zero 8\n", "").encode())
        self.fx.write_store({"lib/arm64-v8a/libr15.so": b"unused: no row links it"})
        self.assertEqual(self.configure().returncode, 0)
        built = self.build()
        self.assertNotEqual(built.returncode, 0)
        self.assertIn("_dd_b is a :got: operand but not a label defined in the file", squash(built.stdout + built.stderr))
        self.assertFalse((self.fx.build / "SHA256SUMS").exists())


if __name__ == "__main__":
    unittest.main()
