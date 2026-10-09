#!/usr/bin/env python3
"""tools/check_quest_static_init.sh against canned llvm-nm / llvm-readelf output.

The stubs stand in for the NDK tools so the parsing and the fail-closed paths are tested without
an NDK. The real tools run in `just build-android`.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "check_quest_static_init.sh"

SECTIONS = """Section Headers:
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [14] .fini_array       FINI_ARRAY      0000000000064cd8 063cd8 000008 00  WA  0   0  8
  [15] .init_array       INIT_ARRAY      0000000000064ce0 063ce0 000018 00  WA  0   0  8
"""
RELOCS = """Relocation section '.rela.dyn' at offset 0x1610 contains 3 entries:
    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend
0000000000064cd8  0000000000000403 R_AARCH64_RELATIVE                20410
0000000000064ce0  0000000000000403 R_AARCH64_RELATIVE                5c138
0000000000064ce8  0000000000000403 R_AARCH64_RELATIVE                5c608
0000000000064cf0  0000000000000403 R_AARCH64_RELATIVE                2047c
"""
SYMBOLS = """000000000005c138 t init_have_lse_atomics
000000000005c608 t init_cpu_features
000000000002047c t _ZL18nevr_sentinel_ctorv
0000000000021774 t _GLOBAL__sub_I_extra.cpp
0000000000021800 t __cxx_global_var_init
"""
CLEAN_OBJECT = "0000000000000000 T something\n                 U puts\n"


class CheckQuestStaticInitTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="quest-static-init-")
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.fix = self.root / "fix"
        self.fix.mkdir()
        (self.fix / "sections").write_text(SECTIONS)
        (self.fix / "relocs").write_text(RELOCS)
        (self.fix / "symbols").write_text(SYMBOLS)
        (self.root / "lib.so").write_text("so")
        self.write_stubs()

    def write_stubs(self, nm_fails_on: str = "") -> None:
        fix = self.fix
        (self.bin / "llvm-readelf").write_text(
            f"""#!/usr/bin/env bash
case "$1" in
  --version) exit 0 ;;
  -S) cat {fix}/sections ;;
  -r) cat {fix}/relocs ;;
esac
"""
        )
        (self.bin / "llvm-nm").write_text(
            f"""#!/usr/bin/env bash
[ "$1" = "--version" ] && exit 0
last="${{@: -1}}"
base="$(basename "$last")"
if [ "$base" = "{nm_fails_on}" ] && [ -n "{nm_fails_on}" ]; then exit 3; fi
if [ "$base" = "lib.so" ]; then cat {fix}/symbols; exit 0; fi
cat {fix}/obj_$base
"""
        )
        for stub in ("llvm-readelf", "llvm-nm"):
            (self.bin / stub).chmod(0o755)

    def add_object(self, name: str, text: str = CLEAN_OBJECT) -> pathlib.Path:
        (self.fix / f"obj_{name}").write_text(text)
        path = self.root / name
        path.write_text("o")
        return path

    def run_check(self, *objects: pathlib.Path, bin_dir: pathlib.Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(SCRIPT), str(bin_dir or self.bin), str(self.root / "lib.so"), *map(str, objects)],
            capture_output=True,
            text=True,
        )

    def test_expected_initializers_and_clean_objects_pass(self) -> None:
        result = self.run_check(self.add_object("a.o"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (3 .init_array entries", result.stdout)

    def test_a_tool_that_cannot_run_fails_and_names_the_path(self) -> None:
        bogus = self.root / "nonexistent"
        result = self.run_check(self.add_object("a.o"), bin_dir=bogus)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(bogus / "llvm-nm"), result.stderr)
        self.assertNotIn("OK", result.stdout)

    def test_a_failing_nm_on_an_object_fails(self) -> None:
        obj = self.add_object("a.o")
        self.write_stubs(nm_fails_on="a.o")
        result = self.run_check(obj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("llvm-nm failed on", result.stderr)

    def test_an_unexpected_init_array_entry_fails(self) -> None:
        (self.fix / "sections").write_text(SECTIONS.replace("000018", "000020"))
        (self.fix / "relocs").write_text(RELOCS + "0000000000064cf8  0000000000000403 R_AARCH64_RELATIVE                21774\n")
        result = self.run_check(self.add_object("a.o"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("_GLOBAL__sub_I_extra.cpp", result.stderr)

    def test_an_inline_variable_initializer_entry_fails(self) -> None:
        (self.fix / "sections").write_text(SECTIONS.replace("000018", "000020"))
        (self.fix / "relocs").write_text(RELOCS + "0000000000064cf8  0000000000000403 R_AARCH64_RELATIVE                21800\n")
        result = self.run_check(self.add_object("a.o"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("__cxx_global_var_init", result.stderr)

    def test_an_entry_with_no_symbol_fails(self) -> None:
        (self.fix / "sections").write_text(SECTIONS.replace("000018", "000020"))
        (self.fix / "relocs").write_text(RELOCS + "0000000000064cf8  0000000000000403 R_AARCH64_RELATIVE                99999\n")
        result = self.run_check(self.add_object("a.o"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("has no symbol", result.stderr)

    def test_a_missing_sentinel_constructor_fails(self) -> None:
        (self.fix / "relocs").write_text(RELOCS.replace("2047c", "5c138"))
        result = self.run_check(self.add_object("a.o"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("nevr_sentinel_ctor is not in .init_array", result.stderr)

    def test_a_slot_count_that_does_not_match_the_relocations_fails(self) -> None:
        (self.fix / "sections").write_text(SECTIONS.replace("000018", "000020"))
        result = self.run_check(self.add_object("a.o"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("relocations were read", result.stderr)

    def test_objects_with_initializers_fail(self) -> None:
        for symbol in ("_GLOBAL__sub_I_x.cpp", "__cxx_global_var_init.3"):
            with self.subTest(symbol=symbol):
                obj = self.add_object("b.o", f"0000000000000000 t {symbol}\n")
                result = self.run_check(obj)
                self.assertEqual(result.returncode, 1)
                self.assertIn(symbol, result.stderr)

    def test_no_objects_is_an_error(self) -> None:
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no object files", result.stderr)


if __name__ == "__main__":
    unittest.main()
