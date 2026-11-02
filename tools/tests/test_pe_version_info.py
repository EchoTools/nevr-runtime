"""The VERSIONINFO of BugSplat64.dll, nevr-runtime.dll and echovr_server.exe (#850): tools/pe_version_info.py and the CMake template.

The oracle is the real resource compiler: RenderedTemplateTest fills cmake/nevr_version_info.rc.in the way
nevr_add_version_info does, builds it with MinGW windres and gcc, and reads the result back with the reader.
Every mutation of a field is a change to that rendered .rc, built the same way, so a field the reader or the
checker stops pinning fails here. These tests skip when no MinGW toolchain is installed. SyntheticPeTest needs no
toolchain. BuiltFilesTest reads the files `just build` made and skips when they are absent."""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import pe_version_info as vi  # noqa: E402
from pe_version_fixtures import pe_with_resources, resource_tree, shipped_pe, version_block  # noqa: E402
import just_recipe  # noqa: E402

TOOL = REPO / "tools" / "pe_version_info.py"
TEMPLATE = REPO / "cmake" / "nevr_version_info.cmake"
GCC = shutil.which("x86_64-w64-mingw32-gcc")
WINDRES = shutil.which("x86_64-w64-mingw32-windres")
STRIP = shutil.which("x86_64-w64-mingw32-strip")
CMAKE = shutil.which("cmake")
TOOLCHAIN = bool(GCC and WINDRES and CMAKE)

DEV_VERSION = "5.5.2-dev.3+abcdef0"
RELEASE_VERSION = "5.5.1"
COMMIT = "0123456789abcdef0123456789abcdef01234567"

RENDER_SCRIPT = """\
include("{template}")
nevr_version_numeric(NEVR_VI_NUMERIC_VERSION "${{MAJOR}}" "${{MINOR}}" "${{PATCH}}" "${{TWEAK}}")
set(NEVR_VI_FILETYPE "${{FILETYPE}}")
set(NEVR_VI_FILE_DESCRIPTION "${{DESCRIPTION}}")
set(NEVR_VI_ORIGINAL_FILENAME "${{ORIGINAL}}")
set(NEVR_VI_VERSION "${{VERSION}}")
configure_file("{rc_in}" "${{OUT}}" @ONLY)
"""


def render_rc(work: Path, *, original: str, version: str, major: int, minor: int, patch: int, tweak: int) -> str:
    """cmake/nevr_version_info.rc.in filled by CMake itself, with the numbers nevr_version_numeric makes."""
    spec = vi.EXPECTED_FILES[original]
    work.mkdir(parents=True, exist_ok=True)
    script = work / "render.cmake"
    script.write_text(RENDER_SCRIPT.format(template=TEMPLATE, rc_in=REPO / "cmake" / "nevr_version_info.rc.in"))
    out = work / (original + ".rc")
    subprocess.run(
        [CMAKE, f"-DMAJOR={major}", f"-DMINOR={minor}", f"-DPATCH={patch}", f"-DTWEAK={tweak}",
         f"-DFILETYPE={'VFT_DLL' if spec['file_type'] == vi.VFT_DLL else 'VFT_APP'}",
         f"-DDESCRIPTION={spec['FileDescription']}", f"-DORIGINAL={original}", f"-DVERSION={version}",
         f"-DOUT={out}", "-P", str(script)],
        check=True, capture_output=True, text=True)
    return out.read_text(encoding="utf-8")


def build_pe(work: Path, name: str, rc_text: str | None, *, identity_version: str | None = None) -> Path:
    """A PE file called `name` carrying `rc_text` as its resources (None: no resources): windres to COFF, linked
    with no CRT."""
    work.mkdir(parents=True, exist_ok=True)
    rc_objects = []
    if rc_text is not None:
        rc = work / "fixture.rc"
        rc.write_text(rc_text, encoding="utf-8")
        rc_objects = [str(work / "fixture_rc.o")]
        subprocess.run([WINDRES, "-O", "coff", "-i", str(rc), "-o", rc_objects[0]], check=True, capture_output=True, text=True)
    source = work / "fixture.c"
    literal = f'"\\0NEVR-BUILD {identity_version} {COMMIT}"' if identity_version else '"\\0"'
    source.write_text(f"const char nevr_fixture_identity[] __attribute__((used)) = {literal};\n")
    exe = name.endswith(".exe")
    out = work / name
    subprocess.run([GCC, *([] if exe else ["-shared"]), "-nostdlib", "-nostartfiles", "-Wl,-e,0",
                    str(source), *rc_objects, "-o", str(out)], check=True, capture_output=True, text=True)
    return out


@unittest.skipUnless(TOOLCHAIN, "needs MinGW gcc, windres and cmake")
class RenderedTemplateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def dev_rc(self, name: str, sub: str = "dev") -> str:
        return render_rc(self.root / sub, original=name, version=DEV_VERSION, major=5, minor=5, patch=1, tweak=3)

    def build(self, name: str, rc_text: str | None, sub: str, identity_version: str | None = DEV_VERSION) -> Path:
        return build_pe(self.root / sub, name, rc_text, identity_version=identity_version)

    def test_development_dll_reads_back_every_field(self):
        dll = self.build("BugSplat64.dll", self.dev_rc("BugSplat64.dll"), "a")
        info = vi.read_version_info(dll)
        self.assertEqual(info.file_version, (5, 5, 1, 3))
        self.assertEqual(info.product_version, (5, 5, 1, 3))
        self.assertEqual((info.file_os, info.file_type, info.file_subtype), (0x40004, 0x2, 0))
        self.assertEqual(info.translations, [(0x0409, 1200)])
        self.assertEqual(info.tables, {"040904B0": {
            "CompanyName": "Andrew Bates",
            "FileDescription": "nEVR host for Echo VR",
            "FileVersion": DEV_VERSION,
            "InternalName": "BugSplat64.dll",
            "LegalCopyright": "© 2026 Andrew Bates <a@sprock.io>",
            "OriginalFilename": "BugSplat64.dll",
            "ProductName": "nEVR runtime",
            "ProductVersion": DEV_VERSION,
        }})
        self.assertEqual(vi.problems_for(dll, DEV_VERSION), [])

    def test_release_server_exe_reads_back_every_field(self):
        rc = render_rc(self.root / "b", original="echovr_server.exe", version=RELEASE_VERSION, major=5, minor=5, patch=1, tweak=0)
        exe = self.build("echovr_server.exe", rc, "b", identity_version=None)
        info = vi.read_version_info(exe)
        self.assertEqual(info.file_version, (5, 5, 1, 0))
        self.assertEqual(info.file_type, 0x1)
        strings = info.tables["040904B0"]
        self.assertEqual(strings["FileDescription"], "nEVR dedicated server launcher")
        self.assertEqual(strings["OriginalFilename"], "echovr_server.exe")
        self.assertEqual(strings["InternalName"], "echovr_server.exe")
        self.assertEqual(strings["FileVersion"], RELEASE_VERSION)
        self.assertEqual(vi.problems_for(exe, RELEASE_VERSION), [])

    def test_the_numeric_version_sorts_like_the_semver_string(self):
        self.assertEqual(vi.numeric_version("5.5.1"), (5, 5, 1, 0))
        self.assertEqual(vi.numeric_version(DEV_VERSION), (5, 5, 1, 3))
        self.assertLess(vi.numeric_version("5.5.1"), vi.numeric_version(DEV_VERSION))
        self.assertLess(vi.numeric_version(DEV_VERSION), vi.numeric_version("5.5.2"))

    def test_a_changed_field_is_named_in_the_problem(self):
        rc = self.dev_rc("BugSplat64.dll", "m")
        mutations = {
            "CompanyName": ('VALUE "CompanyName", "Andrew Bates"', 'VALUE "CompanyName", "Someone Else"'),
            "FileDescription": ('"nEVR host for Echo VR"', '"nEVR runtime"'),
            "FileVersion": (f'VALUE "FileVersion", "{DEV_VERSION}"', 'VALUE "FileVersion", "5.5.1"'),
            "InternalName": ('VALUE "InternalName", "BugSplat64.dll"', 'VALUE "InternalName", "bugsplat64.dll"'),
            "LegalCopyright": ("© 2026 Andrew Bates", "(c) 2026 Andrew Bates"),
            "OriginalFilename": ('VALUE "OriginalFilename", "BugSplat64.dll"', 'VALUE "OriginalFilename", "x.dll"'),
            "ProductName": ('VALUE "ProductName", "nEVR runtime"', 'VALUE "ProductName", "nEVR"'),
            "ProductVersion": (f'VALUE "ProductVersion", "{DEV_VERSION}"', 'VALUE "ProductVersion", "5.5.1"'),
        }
        for field, (old, new) in mutations.items():
            with self.subTest(field=field):
                self.assertIn(old, rc)
                dll = self.build("BugSplat64.dll", rc.replace(old, new), "m_" + field)
                found = vi.problems_for(dll, DEV_VERSION)
                self.assertTrue(any(f"string {field} " in p for p in found), found)

    def test_a_missing_or_extra_string_is_a_problem(self):
        rc = self.dev_rc("BugSplat64.dll", "x")
        missing = rc.replace('      VALUE "ProductName", "nEVR runtime"\n', "")
        self.assertNotEqual(missing, rc)
        found = vi.problems_for(self.build("BugSplat64.dll", missing, "x_missing"), DEV_VERSION)
        self.assertTrue(any("string ProductName is None" in p for p in found), found)
        extra = rc.replace('      VALUE "ProductName"', '      VALUE "Comments", "hi"\n      VALUE "ProductName"')
        found = vi.problems_for(self.build("BugSplat64.dll", extra, "x_extra"), DEV_VERSION)
        self.assertTrue(any("string Comments is 'hi'" in p for p in found), found)

    def test_a_wrong_numeric_version_or_fixed_field_is_a_problem(self):
        rc = self.dev_rc("BugSplat64.dll", "n")
        cases = {
            "FILEVERSION": ("FILEVERSION 5,5,1,3", "FILEVERSION 5,5,1,4"),
            "PRODUCTVERSION": ("PRODUCTVERSION 5,5,1,3", "PRODUCTVERSION 5,5,2,3"),
            "FILEOS": ("FILEOS VOS_NT_WINDOWS32", "FILEOS VOS__WINDOWS32"),
            "FILETYPE": ("FILETYPE VFT_DLL", "FILETYPE VFT_APP"),
            "FILESUBTYPE": ("FILESUBTYPE VFT2_UNKNOWN", "FILESUBTYPE 1"),
            "Translation": ("VALUE \"Translation\", 0x0409, 1200", "VALUE \"Translation\", 0x0409, 1252"),
        }
        for label, (old, new) in cases.items():
            with self.subTest(label=label):
                self.assertIn(old, rc)
                found = vi.problems_for(self.build("BugSplat64.dll", rc.replace(old, new), "n_" + label), DEV_VERSION)
                self.assertTrue(any(label in p for p in found), found)

    def test_a_second_string_table_is_a_problem(self):
        rc = self.dev_rc("BugSplat64.dll", "t")
        start = rc.index('    BLOCK "040904B0"')
        end = rc.index("    END\n", start) + len("    END\n")
        second = rc[start:end].replace("040904B0", "040704B0")
        found = vi.problems_for(self.build("BugSplat64.dll", rc[:end] + second + rc[end:], "t_two"), DEV_VERSION)
        self.assertTrue(any("string tables" in p for p in found), found)

    def test_two_version_resources_are_refused(self):
        rc = self.dev_rc("BugSplat64.dll", "d")
        body = rc.split("#pragma code_page(65001)\n", 1)[1]
        doubled = rc + "LANGUAGE 0x07, 0x01\n" + body
        with self.assertRaisesRegex(vi.PeVersionError, "2 VERSIONINFO resources"):
            vi.read_version_info(self.build("BugSplat64.dll", doubled, "d_two"))

    def test_a_file_with_no_resource_is_refused(self):
        dll = self.build("BugSplat64.dll", None, "e")
        found = vi.problems_for(dll, DEV_VERSION)
        self.assertEqual(len(found), 1)
        self.assertRegex(found[0], r"BugSplat64\.dll: .*(no resource directory|0 VERSIONINFO)")

    def test_stripping_the_file_keeps_the_resource(self):
        if not STRIP:
            self.skipTest("needs MinGW strip")
        dll = self.build("BugSplat64.dll", self.dev_rc("BugSplat64.dll", "s"), "s")
        subprocess.run([STRIP, "--strip-all", str(dll)], check=True, capture_output=True, text=True)
        self.assertEqual(vi.problems_for(dll, DEV_VERSION), [])

    def test_check_takes_the_version_from_the_dlls_identity_literal(self):
        dll = self.build("BugSplat64.dll", self.dev_rc("BugSplat64.dll", "c1"), "c_dll")
        exe = self.build("echovr_server.exe", self.dev_rc("echovr_server.exe", "c2"), "c_exe", identity_version=None)
        self.assertEqual(vi.check(dll, exe), [])
        self.assertEqual(vi.check(dll, exe, DEV_VERSION), [])
        found = vi.check(dll, exe, RELEASE_VERSION)
        self.assertTrue(any("BugSplat64.dll" in p for p in found) and any("echovr_server.exe" in p for p in found), found)

    def test_check_fails_when_the_dll_states_another_version(self):
        dll = self.build("BugSplat64.dll", self.dev_rc("BugSplat64.dll", "w1"), "w_dll", identity_version="5.5.1")
        exe = self.build("echovr_server.exe", self.dev_rc("echovr_server.exe", "w2"), "w_exe", identity_version=None)
        found = vi.check(dll, exe)
        self.assertTrue(any("FILEVERSION 5.5.1.3 is not 5.5.1.0" in p for p in found), found)

    def test_check_covers_the_runtime_and_binds_it_to_the_hosts_version(self):
        dll = self.build("BugSplat64.dll", self.dev_rc("BugSplat64.dll", "r1"), "r_dll")
        runtime = self.build("nevr-runtime.dll", self.dev_rc("nevr-runtime.dll", "r2"), "r_rt")
        exe = self.build("echovr_server.exe", self.dev_rc("echovr_server.exe", "r3"), "r_exe", identity_version=None)
        self.assertEqual(vi.check(dll, exe, None, runtime), [])
        # a runtime of another build is not this host's pair
        other = self.build("nevr-runtime.dll", self.dev_rc("nevr-runtime.dll", "r4"), "r_other", identity_version="5.5.1")
        found = vi.check(dll, exe, None, other)
        self.assertTrue(any("nevr-runtime.dll: its identity version 5.5.1 is not BugSplat64.dll's" in p for p in found), found)
        # a runtime carrying the host's description is the wrong file
        wrong = self.dev_rc("nevr-runtime.dll", "r5").replace("nEVR runtime for Echo VR", "nEVR host for Echo VR")
        found = vi.check(dll, exe, None, self.build("nevr-runtime.dll", wrong, "r_wrong"))
        self.assertTrue(any("FileDescription" in p and "nevr-runtime.dll" in p for p in found), found)

    def test_check_fails_without_an_identity_literal_and_without_a_version(self):
        dll = self.build("BugSplat64.dll", self.dev_rc("BugSplat64.dll", "i1"), "i_dll", identity_version=None)
        exe = self.build("echovr_server.exe", self.dev_rc("echovr_server.exe", "i2"), "i_exe", identity_version=None)
        found = vi.check(dll, exe)
        self.assertEqual(len(found), 1)
        self.assertIn("0 NEVR-BUILD identity literals", found[0])

    def test_the_cli_exits_zero_on_a_pass_and_one_on_a_failure(self):
        dll = self.build("BugSplat64.dll", self.dev_rc("BugSplat64.dll", "k1"), "k_dll")
        exe = self.build("echovr_server.exe", self.dev_rc("echovr_server.exe", "k2"), "k_exe", identity_version=None)
        ok = subprocess.run([sys.executable, "-I", str(TOOL), "check", "--dll", str(dll), "--exe", str(exe)],
                            capture_output=True, text=True)
        self.assertEqual((ok.returncode, ok.stderr), (0, ""), ok.stdout)
        self.assertIn(f"pe-version-info: OK: BugSplat64.dll and echovr_server.exe carry the pinned VERSIONINFO ({DEV_VERSION})", ok.stdout)
        bad = subprocess.run([sys.executable, "-I", str(TOOL), "check", "--dll", str(dll), "--exe", str(exe),
                              "--version", RELEASE_VERSION], capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1)
        self.assertIn("pe-version-info: FAIL:", bad.stderr)
        shown = subprocess.run([sys.executable, "-I", str(TOOL), "show", str(dll)], capture_output=True, text=True)
        self.assertEqual(shown.returncode, 0)
        self.assertIn("FILEVERSION     5.5.1.3", shown.stdout)
        self.assertIn("040904B0 LegalCopyright = © 2026 Andrew Bates <a@sprock.io>", shown.stdout)

    def test_the_template_text_leaves_the_build_identity_literal_unique(self):
        rendered = self.dev_rc("BugSplat64.dll", "u")
        self.assertNotIn("NEVR-BUILD", rendered)


MINI_PROJECT = """\
cmake_minimum_required(VERSION 3.20)
project(mini LANGUAGES C)
set(PROJECT_VERSION "{version}")
set(PROJECT_VERSION_MAJOR 5)
set(PROJECT_VERSION_MINOR 5)
set(PROJECT_VERSION_PATCH 1)
set(PROJECT_VERSION_TWEAK 3)
enable_language(RC)
include("${{CMAKE_SOURCE_DIR}}/cmake/nevr_version_info.cmake")
add_library(runtime SHARED runtime.c)
set_target_properties(runtime PROPERTIES OUTPUT_NAME "BugSplat64" PREFIX "")
nevr_add_version_info(runtime {runtime_args})
add_library(nevr_runtime_dll SHARED runtime.c)
set_target_properties(nevr_runtime_dll PROPERTIES OUTPUT_NAME "nevr-runtime" PREFIX "")
nevr_add_version_info(nevr_runtime_dll FILE_DESCRIPTION "nEVR runtime for Echo VR" ORIGINAL_FILENAME "nevr-runtime.dll" TYPE DLL)
add_executable(launcher launcher.c)
set_target_properties(launcher PROPERTIES OUTPUT_NAME "echovr_server")
nevr_add_version_info(launcher FILE_DESCRIPTION "nEVR dedicated server launcher" ORIGINAL_FILENAME "echovr_server.exe" TYPE APP)
"""
RUNTIME_ARGS = 'FILE_DESCRIPTION "nEVR host for Echo VR" ORIGINAL_FILENAME "BugSplat64.dll" TYPE DLL'


@unittest.skipUnless(TOOLCHAIN and shutil.which("ninja"), "needs MinGW gcc, windres, cmake and ninja")
class AddVersionInfoFunctionTest(unittest.TestCase):
    """nevr_add_version_info in a one-DLL, one-exe project configured with the repo's MinGW toolchain."""

    def configure(self, root: Path, runtime_args: str = RUNTIME_ARGS, version: str = DEV_VERSION):
        (root / "cmake").mkdir(parents=True)
        for name in ("nevr_version_info.cmake", "nevr_version_info.rc.in"):
            shutil.copyfile(REPO / "cmake" / name, root / "cmake" / name)
        (root / "CMakeLists.txt").write_text(MINI_PROJECT.format(version=version, runtime_args=runtime_args))
        (root / "runtime.c").write_text("int __stdcall DllMain(void *h, unsigned long r, void *p) { return 1; }\n")
        (root / "launcher.c").write_text("int main(void) { return 0; }\n")
        return subprocess.run([CMAKE, "-S", str(root), "-B", str(root / "build"), "-G", "Ninja",
                               f"-DCMAKE_TOOLCHAIN_FILE={REPO / 'cmake' / 'toolchain-mingw64.cmake'}"],
                              capture_output=True, text=True)

    def test_both_targets_carry_the_resource_the_function_makes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            configured = self.configure(root)
            self.assertEqual(configured.returncode, 0, configured.stderr)
            built = subprocess.run([CMAKE, "--build", str(root / "build")], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for name in vi.EXPECTED_FILES:
                with self.subTest(name=name):
                    self.assertEqual(vi.problems_for(root / "build" / name, DEV_VERSION), [])
            self.assertEqual(vi.read_version_info(root / "build" / "BugSplat64.dll").file_version, (5, 5, 1, 3))

    def test_a_call_that_would_write_a_wrong_resource_fails_the_configure(self):
        cases = {
            "missing description": ('ORIGINAL_FILENAME "BugSplat64.dll" TYPE DLL', "FILE_DESCRIPTION is required"),
            "missing file name": ('FILE_DESCRIPTION "x" TYPE DLL', "ORIGINAL_FILENAME is required"),
            "missing type": ('FILE_DESCRIPTION "x" ORIGINAL_FILENAME "BugSplat64.dll"', "TYPE is required"),
            "unknown type": ('FILE_DESCRIPTION "x" ORIGINAL_FILENAME "BugSplat64.dll" TYPE LIB', "TYPE is DLL or APP"),
            "quote in text": ('FILE_DESCRIPTION "say \\"hi\\"" ORIGINAL_FILENAME "BugSplat64.dll" TYPE DLL', "quote or a backslash"),
            "backslash in text": ('FILE_DESCRIPTION "a\\\\b" ORIGINAL_FILENAME "BugSplat64.dll" TYPE DLL', "quote or a backslash"),
        }
        for label, (args, message) in cases.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as tmp:
                configured = self.configure(Path(tmp), runtime_args=args)
                self.assertNotEqual(configured.returncode, 0, label)
                self.assertIn(message, configured.stderr)

    def test_a_version_with_a_quote_fails_the_configure(self):
        with tempfile.TemporaryDirectory() as tmp:
            configured = self.configure(Path(tmp), version='5.5.1\\"x')  # CMake source: a version with a literal quote
            self.assertNotEqual(configured.returncode, 0)
            self.assertIn("quote or a backslash", configured.stderr)


@unittest.skipUnless(CMAKE, "needs cmake")
class NumericVersionCmakeTest(unittest.TestCase):
    def run_numeric(self, major, minor, patch, distance):
        script = f'include("{TEMPLATE}")\nnevr_version_numeric(OUT "{major}" "{minor}" "{patch}" "{distance}")\nmessage("OUT=${{OUT}}")\n'
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "numeric.cmake"
            path.write_text(script)
            return subprocess.run([CMAKE, "-P", str(path)], capture_output=True, text=True)

    def test_the_four_parts_are_joined_and_an_empty_distance_is_zero(self):
        self.assertIn("OUT=5,5,1,3", self.run_numeric(5, 5, 1, 3).stderr)
        self.assertIn("OUT=5,5,1,0", self.run_numeric(5, 5, 1, "").stderr)
        self.assertIn("OUT=65535,0,0,65535", self.run_numeric(65535, 0, 0, 65535).stderr)

    def test_a_part_above_sixteen_bits_fails_the_configure(self):
        for args in ((65536, 0, 0, 0), (5, 65536, 0, 0), (5, 5, 65536, 0), (5, 5, 1, 65536)):
            with self.subTest(args=args):
                run = self.run_numeric(*args)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn("65535", run.stderr)

    def test_a_part_that_is_not_a_whole_number_fails_the_configure(self):
        for args in (("x", 0, 0, 0), (5, -1, 0, 0), (5, 5, "1.5", 0), (5, 5, 1, "01")):
            with self.subTest(args=args):
                self.assertNotEqual(self.run_numeric(*args).returncode, 0)


class VersionTextTest(unittest.TestCase):
    def test_numeric_version_of_release_and_development_strings(self):
        self.assertEqual(vi.numeric_version("5.5.0"), (5, 5, 0, 0))
        self.assertEqual(vi.numeric_version("5.5.1-dev.0+abcdef0"), (5, 5, 0, 0))
        self.assertEqual(vi.numeric_version("5.5.2-dev.12+" + COMMIT), (5, 5, 1, 12))

    def test_numeric_version_refuses_what_is_not_a_version(self):
        for text in ("", "5.5", "5.5.1.0", "v5.5.1", "5.5.1-dev", "5.5.1-dev.3", "5.5.1-dev.3+xyz", "5.05.1", "5.5.0-dev.1+abcdef0",
                     "70000.0.0", "5.5.2-dev.70000+abcdef0"):
            with self.subTest(text=text):
                with self.assertRaises(vi.PeVersionError):
                    vi.numeric_version(text)


class SyntheticPeTest(unittest.TestCase):
    """Hand-built resources, so the reader's failure paths need no toolchain."""

    def write(self, data: bytes, name: str = "BugSplat64.dll") -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / name
        path.write_bytes(data)
        return path

    def fixed(self, signature=vi.FIXED_SIGNATURE) -> bytes:
        return struct.pack("<13I", signature, 0x10000, 5 << 16 | 5, 1 << 16 | 3, 5 << 16 | 5, 1 << 16 | 3, 0x3F, 0,
                           vi.VOS_NT_WINDOWS32, vi.VFT_DLL, 0, 0, 0)

    def good_blob(self) -> bytes:
        return version_block("VS_VERSION_INFO", self.fixed())

    def test_a_minimal_block_with_no_strings_parses_and_then_fails_the_pins(self):
        path = self.write(pe_with_resources(resource_tree(self.good_blob())))
        info = vi.read_version_info(path)
        self.assertEqual(info.file_version, (5, 5, 1, 3))
        self.assertEqual(info.tables, {})
        found = vi.problems_for(path, DEV_VERSION)
        self.assertTrue(any("string tables [] are not" in p for p in found), found)
        self.assertTrue(any("Translation [] is not" in p for p in found), found)

    def test_a_wrong_fixed_signature_is_refused(self):
        blob = version_block("VS_VERSION_INFO", self.fixed(signature=0x12345678))
        with self.assertRaisesRegex(vi.PeVersionError, "signature"):
            vi.read_version_info(self.write(pe_with_resources(resource_tree(blob))))

    def test_a_wrong_block_name_is_refused(self):
        blob = version_block("VS_VERSION_INFX", self.fixed())
        with self.assertRaisesRegex(vi.PeVersionError, "not VS_VERSION_INFO"):
            vi.read_version_info(self.write(pe_with_resources(resource_tree(blob))))

    def test_a_block_longer_than_its_data_is_refused(self):
        blob = bytearray(self.good_blob())
        struct.pack_into("<H", blob, 0, len(blob) + 8)
        with self.assertRaisesRegex(vi.PeVersionError, "outside its parent"):
            vi.read_version_info(self.write(pe_with_resources(resource_tree(bytes(blob)))))

    def test_a_truncated_file_is_refused_not_misread(self):
        data = pe_with_resources(resource_tree(self.good_blob()))
        for cut in (0x100, 0x1F0, len(data) - 8):
            with self.subTest(cut=cut):
                with self.assertRaises(vi.PeVersionError):
                    vi.read_version_info(self.write(data[:cut]))

    def test_a_file_that_is_not_a_pe_is_refused(self):
        for data in (b"", b"MZ", b"not a pe file at all" * 8, b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40) + b"XX\0\0" + b"\0" * 300):
            with self.subTest(size=len(data)):
                with self.assertRaises(vi.PeVersionError):
                    vi.read_version_info(self.write(data))

    def test_a_pe_without_a_resource_directory_is_refused(self):
        path = self.write(pe_with_resources(b"\0" * 64))
        data = bytearray(path.read_bytes())
        struct.pack_into("<II", data, 0x40 + 24 + 112 + 2 * 8, 0, 0)
        path.write_bytes(bytes(data))
        with self.assertRaisesRegex(vi.PeVersionError, "no resource directory"):
            vi.read_version_info(path)

    def test_the_synthetic_shipped_files_pass_the_pins_and_a_wrong_version_fails(self):
        for name in vi.EXPECTED_FILES:
            with self.subTest(name=name):
                path = self.write(shipped_pe(name, DEV_VERSION, b"overlay"), name)
                self.assertEqual(vi.problems_for(path, DEV_VERSION), [])
                self.assertTrue(any("FILEVERSION" in p for p in vi.problems_for(path, RELEASE_VERSION)))

    def test_two_identity_literals_or_none_are_refused(self):
        literal = f"NEVR-BUILD {DEV_VERSION} {COMMIT}".encode()
        for count in (0, 2):
            with self.subTest(count=count):
                dll = self.write(shipped_pe("BugSplat64.dll", DEV_VERSION, b"\0" + b"\0".join([literal] * count) + b"\0"))
                with self.assertRaisesRegex(vi.PeVersionError, f"{count} NEVR-BUILD identity literals"):
                    vi.identity_version(dll)
        one = self.write(shipped_pe("BugSplat64.dll", DEV_VERSION, b"\0" + literal + b"\0"))
        self.assertEqual(vi.identity_version(one), DEV_VERSION)

    def test_a_missing_file_is_a_problem_not_a_crash(self):
        found = vi.problems_for(Path("/nonexistent-dir/BugSplat64.dll"), DEV_VERSION)
        self.assertEqual(len(found), 1)
        self.assertIn("BugSplat64.dll:", found[0])

    def test_a_file_named_other_than_the_two_shipped_ones_is_a_problem(self):
        path = self.write(pe_with_resources(resource_tree(self.good_blob())))
        other = path.with_name("other.dll")
        other.write_bytes(path.read_bytes())
        self.assertEqual(vi.problems_for(other, DEV_VERSION), ["other.dll: not one of ['BugSplat64.dll', 'echovr_server.exe', 'nevr-runtime.dll']"])


class WiringTest(unittest.TestCase):
    """The CMake calls and the verify recipe that make the two files carry the resource (static, no toolchain)."""

    CALL = r"nevr_add_version_info\(\s*{target}\s+FILE_DESCRIPTION\s+\"([^\"]*)\"\s+ORIGINAL_FILENAME\s+\"([^\"]*)\"\s+TYPE\s+(DLL|APP)\s*\)"

    def call(self, path: str, target: str):
        import re
        found = re.findall(self.CALL.format(target=target), (REPO / path).read_text())
        self.assertEqual(len(found), 1, f"{path}: expected one nevr_add_version_info({target} ...) call")
        return found[0]

    def test_each_shipped_target_is_attached_with_the_pinned_description_and_file_name(self):
        for path, target, name, kind in (("src/runtime/CMakeLists.txt", "nevr_host", "BugSplat64.dll", "DLL"),
                                         ("src/runtime/CMakeLists.txt", "nevr_runtime", "nevr-runtime.dll", "DLL"),
                                         ("src/launcher/CMakeLists.txt", "echovr_server", "echovr_server.exe", "APP")):
            with self.subTest(target=target):
                description, original, file_type = self.call(path, target)
                self.assertEqual((description, original, file_type),
                                 (vi.EXPECTED_FILES[name]["FileDescription"], name, kind))

    def test_the_resource_compiler_is_enabled_before_the_module_is_included(self):
        cmake = (REPO / "CMakeLists.txt").read_text()
        self.assertLess(cmake.index("enable_language(RC)"), cmake.index('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nevr_version_info.cmake")'))
        self.assertLess(cmake.index("project(NEVRServer"), cmake.index("enable_language(RC)"))

    def test_the_verify_recipe_reads_both_built_files_back(self):
        recipe = just_recipe.body((REPO / "justfile").read_text(), "verify")
        self.assertIsNotNone(recipe, "justfile no longer has a verify recipe")
        line = ('python3 tools/pe_version_info.py check --dll "build/{{ preset }}/bin/BugSplat64.dll" '
                '--runtime-dll "build/{{ preset }}/bin/nevr-runtime.dll" --exe "build/{{ preset }}/bin/echovr_server.exe"')
        self.assertEqual(recipe.count(line), 1)

    def test_the_release_gate_runs_the_check_on_the_windows_files(self):
        gate = (REPO / "tools" / "package_release.py").read_text()
        self.assertIn("pe_version_info.check(dll, server, args.version or None, runtime)", gate)


class BuiltFilesTest(unittest.TestCase):
    """The files a `just build` made (build/mingw-release/bin), read back against their own identity literal."""

    BIN = REPO / "build" / "mingw-release" / "bin"

    def test_the_built_dlls_and_server_exe_carry_the_pinned_resource(self):
        dll, runtime, exe = self.BIN / "BugSplat64.dll", self.BIN / "nevr-runtime.dll", self.BIN / "echovr_server.exe"
        if not (dll.is_file() and runtime.is_file() and exe.is_file()):
            self.skipTest(f"{self.BIN} has no BugSplat64.dll, nevr-runtime.dll and echovr_server.exe; run `just build` first")
        self.assertEqual(vi.check(dll, exe, None, runtime), [])


if __name__ == "__main__":
    unittest.main()
