"""The combat script libraries are compiled from a libsrc directory the owner supplies (tools/build_quest_combat_libs.py).

The compiler, the static-init check and the store APK are stand-ins written into a temporary directory: nothing from a real
libsrc, no NDK and no game file is used. The tests assert the command each LIBS.json row produces, and the refusals.
"""

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "build_quest_combat_libs.py"
H1 = "0123456789abcdef"
H2 = "fedcba9876543210"

# A compiler stand-in: records its argv in $CALLS, writes the file after -o, and fails when the source says FAIL.
STUB_CLANG = """#!/usr/bin/env python3
import os, sys
argv = sys.argv[1:]
with open(os.environ["CALLS"], "a") as f:
    f.write("clang " + " ".join(argv) + "\\n")
src = [a for a in argv if a.endswith(".cpp")][0]
if "FAIL" in open(src).read():
    print("error: stub compiler refused", file=sys.stderr)
    sys.exit(3)
open(argv[argv.index("-o") + 1], "wb").write(b"ELF " + os.path.basename(src).encode())
"""
# The static-init check stand-in: records its argv in $CALLS and passes.
STUB_CHECK = """#!/bin/sh
echo "check $*" >> "$CALLS"
"""
STOCK_BYTES = b"stock libr15"


class BuildCombatLibs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.libsrc = self.root / "libsrc"
        self.out = self.root / "out"
        self.calls = self.root / "calls.log"
        self.clang = self.root / "clang"
        self.clang.write_text(STUB_CLANG)
        self.check = self.root / "check.sh"
        self.check.write_text(STUB_CHECK)
        for path in (self.clang, self.check):
            path.chmod(0o755)
        self.store = self.root / "store.apk"
        with zipfile.ZipFile(self.store, "w") as zf:
            zf.writestr("lib/arm64-v8a/libr15.so", STOCK_BYTES)
            zf.writestr("lib/arm64-v8a/libovrplatformloader.so", b"loader")
        self.write_libsrc(LINKS={H1: {"asm": False, "libr15": True, "ldl": False},
                                 H2: {"asm": True, "libr15": False, "ldl": True}})

    def write_libsrc(self, LINKS, names=None):
        self.libsrc.mkdir(exist_ok=True)
        libs = {}
        for h, link in LINKS.items():
            key = (names or {}).get(h, f"lib{h}.so")
            libs[key] = {"md5": "0" * 32, "path": f"x/{key}", "link": link}
            d = self.libsrc / h
            d.mkdir(exist_ok=True)
            (d / f"{h}.cpp").write_text(f"// {h}\n")
            if link["asm"]:
                (d / f"{h}.donor.s").write_text(".text\n")
        (self.libsrc / "LIBS.json").write_text(json.dumps({"libs": libs}))

    def run_tool(self, libsrc=None):
        env = {**os.environ, "CALLS": str(self.calls)}
        return subprocess.run(
            [sys.executable, "-I", str(TOOL), "--libsrc", str(libsrc or self.libsrc), "--store-apk", str(self.store),
             "--out", str(self.out), "--ndk-clang", str(self.clang), "--llvm-bin", str(self.root),
             "--check-script", str(self.check)],
            capture_output=True, text=True, env=env)

    def calls_made(self):
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def test_each_row_is_compiled_with_the_flags_its_link_row_names(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        # The q15t source root is the libsrc directory's parent; its tools/scripts holds regen_rt.h (BUILD.md step 4).
        inc = f"-I{pathlib.Path(self.tmp.name).resolve() / 'tools' / 'scripts'}"
        stock = f"-L{self.out / 'stock'}"
        clang = [c for c in self.calls_made() if c.startswith("clang ")]
        self.assertEqual(clang[0], " ".join([
            "clang", "-shared", "-fPIC", "-O2", "-nostdlib++", inc, "-Wl,--gc-sections", "-Wl,--allow-shlib-undefined",
            str(self.libsrc / H1 / f"{H1}.cpp"), stock, "-l:libr15.so",
            "-o", str(self.out / "work" / H1 / f"lib{H1}.so")]))
        self.assertEqual(clang[1], " ".join([
            "clang", "-shared", "-fPIC", "-O2", "-nostdlib++", inc, "-Wl,--gc-sections", "-Wl,--allow-shlib-undefined",
            str(self.libsrc / H2 / f"{H2}.cpp"), str(self.libsrc / H2 / f"{H2}.donor.s"), "-ldl",
            "-o", str(self.out / "work" / H2 / f"lib{H2}.so")]))

    def test_the_stock_libr15_is_taken_from_the_store_apk_and_the_outputs_are_checked_and_summed(self):
        self.assertEqual(self.run_tool().returncode, 0)
        self.assertEqual((self.out / "stock" / "libr15.so").read_bytes(), STOCK_BYTES)
        sums = (self.out / "SHA256SUMS").read_text().splitlines()
        want = []
        for h in (H1, H2):
            data = (self.out / f"lib{h}.so").read_bytes()
            want.append(f"{hashlib.sha256(data).hexdigest()}  lib{h}.so")
        self.assertEqual(sums, sorted(want, key=lambda line: line.split()[1]))
        checks = [c for c in self.calls_made() if c.startswith("check ")]
        self.assertEqual(checks, [f"check --plugin {self.root} {self.out / 'work' / h / f'lib{h}.so'}" for h in (H1, H2)])

    def test_a_missing_link_row_refuses_before_anything_is_compiled(self):
        self.write_libsrc(LINKS={H1: {"asm": False, "libr15": True, "ldl": False}})
        data = json.loads((self.libsrc / "LIBS.json").read_text())
        del data["libs"][f"lib{H1}.so"]["link"]
        (self.libsrc / "LIBS.json").write_text(json.dumps(data))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn('LIBS.json row lib0123456789abcdef.so has no "link" object', result.stderr)
        self.assertEqual(self.calls_made(), [])

    def test_a_missing_libsrc_directory_or_LIBS_json_is_refused(self):
        result = self.run_tool(libsrc=self.root / "nowhere")
        self.assertEqual(result.returncode, 1)
        self.assertIn("libsrc is not a directory", result.stderr)
        (self.libsrc / "LIBS.json").unlink()
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("no LIBS.json in", result.stderr)
        self.assertEqual(self.calls_made(), [])

    def test_an_output_not_named_lib_hash16_so_is_refused(self):
        for bad in ("libfoo.so", f"lib{H1}.so.bak", "lib0123456789ABCDEF.so", f"lib{H1[:-1]}.so"):
            with self.subTest(name=bad):
                self.write_libsrc(LINKS={H1: {"asm": False, "libr15": True, "ldl": False}}, names={H1: bad})
                result = self.run_tool()
                self.assertEqual(result.returncode, 1)
                self.assertIn(f"LIBS.json key {bad!r} is not named lib<hash16>.so", result.stderr)
                self.assertEqual(self.calls_made(), [])

    def test_a_compiler_failure_names_the_library_and_its_log(self):
        (self.libsrc / H1 / f"{H1}.cpp").write_text("FAIL\n")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"compiling lib{H1}.so failed (exit 3)", result.stderr)
        self.assertTrue((self.out / "work" / H1 / "build.log").is_file())

    def test_a_store_apk_without_libr15_is_refused(self):
        with zipfile.ZipFile(self.store, "w") as zf:
            zf.writestr("lib/arm64-v8a/libovrplatformloader.so", b"loader")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("the store APK has no lib/arm64-v8a/libr15.so", result.stderr)

    def test_the_recipe_refuses_without_the_libsrc_directory_and_takes_it_from_the_environment(self):
        text = (ROOT / "justfile").read_text()
        start = text.index("\nbuild-quest-combat-libs ")
        recipe = text[start:text.index("\n# One unattended autopilot run", start)]
        self.assertIn('libsrc=env("NEVR_COMBAT_LIBSRC", "")', recipe.splitlines()[1])
        self.assertIn("NEVR_COMBAT_LIBSRC names no libsrc directory", recipe)
        self.assertIn("tools/build_quest_combat_libs.py", recipe)


if __name__ == "__main__":
    unittest.main()
