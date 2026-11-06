"""#2053 (ADR 0041): the q15t plugin's host tests are CMake targets, and no script that builds them is left.

Before, `src/quest/tests/q15t/run_host_tests.sh` and the scripts under `src/quest/tests/q15t/mod/` ran the host compiler (and the NDK
compiler) by hand, one command line per test. Now `src/quest/tests/q15t/CMakeLists.txt` and `mod/<module>.cmake` build them, a
preset configures them and `just test-quest-q15t` configures, builds and runs ctest. Three things are pinned here:

1. Neither those recipes nor those scripts contain a compiler or linker command (the list ADR 0041 gives for rule 3), and the old
   scripts are gone.
2. The host configure registers the same tests the scripts ran (the names below, one per program run and one for the symbol check),
   and every test source in the directory is compiled by some target, so a deleted target or a dropped source is noticed.
3. The NDK checks the scripts ran (the static-initializer test per module, the two thunk checks, the qemu run) are still registered.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
Q15T = ROOT / "src/quest/tests/q15t"

# ADR 0041 rule 3 / "Finding a violation" V1: the compilers and linkers a script must not run.
COMPILER = re.compile(
    r"(^|[^A-Za-z0-9_.+-])"
    r"(g\+\+|gcc|clang\+\+|clang|ndk-build|windres|x86_64-w64-mingw32-(gcc|g\+\+|ld|ar|windres)"
    r"|aarch64-linux-android[0-9]*-clang(\+\+)?)($|[^A-Za-z0-9_+-])"
)
# `ld` and `ar` are also words in a format string (`"%ld"`) and a sed expression, so they count only as the command a line runs
LINKER = re.compile(r"(^|[;&|(]|\bexec\s|\$\()\s*(ld|ar)(\s|$)")
# a message printed is not a command: `echo "NDK r26d clang++, ..."` names a compiler and runs none
PRINTED = re.compile(r"""\b(echo|printf)\s+("([^"\\]|\\.)*"|'[^']*')""")

HOST_TESTS = frozenset(
    [
        "core.host_test_0",
        "core.host_test_1",
        "aibots.aibots_test",
        "boosterpool.boosterpool_test",
        "bvh.bvh_core_test",
        "bvh.bvh_test",
        "custctx.custctx_test",
        "droneguard.droneguard_test",
        "dronekill.dronekill_test",
        "dronemuzzle.dronemuzzle_test",
        "ec.ec_test",
        "ec.ec_page_symbol",
        "ec.ec_page_test",
        "ec.ec_page_test_other",
        "ec.ec_test_defaults",
        "ecfx.ecfx_test",
        "ecfx.ecfx_test_missing",
        "ecfx.ecfx_test_defaulton",
        "fxtrail.fxtrail_test",
        "game_tick.game_tick_test",
        "gunconnect.gunconnect_test",
        "hudmeter.hudmeter_test",
        "levelname.levelname_test",
        "music.music_test",
        "peerclamp.peerclamp_test",
        "powerboost.powerboost_test",
        "script_redirect.script_redirect_test",
        "sensorwd.sensorwd_test",
        "shieldgamma.shieldgamma_test",
        "thread_word.thread_word_test",
        "voipdrop.voipdrop_test",
        "wallfire.wallfire_test",
        "weapon_arm.t26_test",
        "wiring.wiring_test",
        "wiring.wiring_bvh_only",
        "wiring.wiring_caps_none",
        "wiring.wiring_caps_FXTRAIL",
        "wiring.wiring_caps_VOIPDROP",
        "wiring.wiring_caps_CUSTCTX",
        "wiring.wiring_caps_ECFX",
        "wiring.wiring_caps_HUDMETER",
        "wiring.wiring_caps_MUSIC",
        "wiring.wiring_caps_SENSORWD",
        "wiring.wiring_caps_SHIELDGAMMA_1",
        "wiring.wiring_caps_SHIELDGAMMA_2",
        "wiring.wiring_caps_DRONEGUARD",
        "wiring.wiring_caps_PEERCLAMP",
        "wiring.wiring_caps_DRONEKILL",
        "wiring.wiring_caps_DRONEMUZZLE",
        "wiring.wiring_caps_GUNCONNECT",
        "wiring.wiring_caps_AIBOTS",
        "wiring.wiring_caps_BVH",
        "wiring.wiring_caps_WALLFIRE",
        "wiring.wiring_caps_LEVELNAME",
        "wiring.wiring_caps_T26",
        "wiring.wiring_caps_POWERBOOST_1",
        "wiring.wiring_caps_POWERBOOST_2",
        "wiring.wiring_caps_POWERBOOST_3",
        "wiring.wiring_caps_EC",
    ]
)
# q15t_arm64_object(<src name>): the NDK static-initializer test and the emulated-TLS test for each of these, one per module the repack
# checks and one for each module that keeps per-thread state (bvh, dronekill, powerboost, sensorwd)
ARM64_OBJECTS = frozenset(
    "bvh dronekill dronemuzzle hudmeter music powerboost sensorwd shieldgamma".split()
)
# the checks that read an object or run a freestanding program, registered by hand in a module file
NDK_CHECKS = {
    "mod/dronemuzzle.cmake": ["dronemuzzle.arm64_thunks"],
    "mod/powerboost.cmake": ["powerboost.arm64_thunk_disassembly", "powerboost.pb_thunk_qemu"],
}


def commands(text: str) -> list[str]:
    """The lines of a script or recipe that can run something: no comment line, no message printed."""
    out = []
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        out.append(PRINTED.sub("", line))
    return out


def compiler_commands(text: str) -> list[str]:
    return [line for line in commands(text) if COMPILER.search(line) or LINKER.search(line)]


def recipe(name: str) -> str:
    """The body of a justfile recipe: its header line to the next line that is neither indented nor blank."""
    lines = (ROOT / "justfile").read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if re.match(rf"^{re.escape(name)}\b.*:\s*$", line))
    body = [lines[start]]
    for line in lines[start + 1 :]:
        if line and not line[0].isspace():
            break
        body.append(line)
    return "\n".join(body)


class NoCompilerCommand(unittest.TestCase):
    def test_the_detector_sees_a_compiler_command(self):
        # the shapes the old scripts used: a host compiler, a variable-quoted NDK compiler path, a linked program
        for line in (
            'g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" "$tests/a_test.cpp" -o "$out/a_test" -ldl',
            '  "$ndk_bin/aarch64-linux-android26-clang++" -std=c++17 -fno-exceptions -c "$here/src/dronekill.cpp" -o "$out/ndk/dronekill.o"',
            'x86_64-w64-mingw32-gcc -shared a.c -o a.dll',
            'exec clang++ -static -nostdlib b.cpp',
            'ar rcs libx.a a.o',
            'cd out && ld -r a.o b.o -o c.o',
        ):
            self.assertTrue(compiler_commands(line), line)

    def test_the_detector_ignores_a_message_and_a_comment(self):
        for line in (
            'echo "arm64 build of x: ok (NDK r26d clang++, no .init_array/.ctors)"',
            '|| { echo "FAIL: NDK clang not found"; exit 1; }',
            "# g++ -std=c++17 is what the old script ran",
            'runm sensorwd t src/sensorwd.cpp \'s/std::snprintf(a, sizeof a, "%ld", x);/y/\'',
        ):
            self.assertEqual(compiler_commands(line), [], line)

    def test_the_old_scripts_are_gone(self):
        self.assertFalse((Q15T / "run_host_tests.sh").exists())
        self.assertEqual(sorted(p.name for p in (Q15T / "mod").glob("*.sh")), [])

    def test_the_recipe_runs_cmake_and_ctest_and_no_compiler(self):
        body = recipe("test-quest-q15t")
        self.assertEqual(compiler_commands(body), [])
        for step in ("cmake --preset q15t-host-tests", "cmake --build --preset q15t-host-tests", "ctest --preset q15t-host-tests",
                     "cmake --preset q15t-ndk-checks", "cmake --build --preset q15t-ndk-checks", "ctest --preset q15t-ndk-checks"):
            self.assertIn(step, body)
        self.assertNotIn("run_host_tests", body)

    def test_the_scripts_that_remain_run_no_compiler(self):
        scripts = [p for d in (Q15T, Q15T / "check", Q15T / "mutants.d", Q15T / "mod") for p in sorted(d.glob("*.sh"))]
        self.assertTrue(any(p.name == "mutants.sh" for p in scripts))
        self.assertTrue(any(p.parent.name == "check" for p in scripts))
        for path in scripts:
            with self.subTest(script=str(path.relative_to(ROOT))):
                self.assertEqual(compiler_commands(path.read_text(encoding="utf-8")), [])

    def test_the_mutation_run_builds_by_cmake(self):
        text = (Q15T / "mutants.sh").read_text(encoding="utf-8")
        for step in ("cmake --preset q15t-host-tests", "-DQ15T_PLUGIN_DIR=", "cmake --build", "ctest --test-dir"):
            self.assertIn(step, text)
        self.assertNotIn("run_host_tests", text)


class HostTestsAreTargets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("cmake", "ctest", "ninja"):
            if shutil.which(tool) is None:
                raise AssertionError(f"{tool} is not on PATH: the q15t host configure cannot be checked")
        cls.build = Path(tempfile.mkdtemp(prefix="q15t-host-configure-"))
        cls.addClassCleanup(shutil.rmtree, cls.build, ignore_errors=True)
        done = subprocess.run(["cmake", "--preset", "q15t-host-tests", "-B", str(cls.build)], cwd=Q15T,
                              capture_output=True, text=True, check=False)
        if done.returncode != 0:
            raise AssertionError(f"configure failed ({done.returncode}):\n{done.stdout}\n{done.stderr}")
        listed = subprocess.run(["ctest", "--test-dir", str(cls.build), "-N"], capture_output=True, text=True, check=True)
        cls.tests = re.findall(r"Test +#\d+: (\S+)", listed.stdout)

    def test_the_configure_registers_the_tests_the_scripts_ran(self):
        self.assertEqual(sorted(set(self.tests) - HOST_TESTS), [], "tests registered but not in HOST_TESTS: add them")
        self.assertEqual(sorted(HOST_TESTS - set(self.tests)), [], "a test the scripts ran is no longer registered")
        self.assertEqual(len(self.tests), len(set(self.tests)))

    def test_every_test_source_is_compiled_by_a_target(self):
        import json

        database = json.loads((self.build / "compile_commands.json").read_text(encoding="utf-8"))
        compiled = {Path(entry["file"]).name for entry in database if Path(entry["file"]).parent == Q15T}
        # compiled only in the NDK configure: the freestanding thunk program
        ndk_only = {"pb_thunk_qemu.cpp"}
        sources = {p.name for p in Q15T.glob("*.cpp")}
        self.assertEqual(sorted(sources - compiled - ndk_only), [], "a .cpp here no target compiles")
        text = (Q15T / "mod/powerboost.cmake").read_text(encoding="utf-8")
        self.assertIn("pb_thunk_qemu.cpp", text)

    def test_a_label_per_module_file(self):
        labels = {t.split(".")[0] for t in self.tests}
        modules = {p.stem for p in (Q15T / "mod").glob("*.cmake")} | {"core"}
        self.assertEqual(sorted(labels - modules), [])
        self.assertEqual(sorted(modules - labels), [], "a module file registers no host test")


class NoEmulatedTls(unittest.TestCase):
    """check/no_emutls.sh: the Quest build targets API 26, where thread_local is emulated (__emutls_v.*) and allocates on first use."""

    SCRIPT = Q15T / "check/no_emutls.sh"

    def run_script(self, nm_output: str, *, nm_exit: int = 0, file_exists: bool = True):
        with tempfile.TemporaryDirectory(prefix="q15t-no-emutls-") as tmp:
            tmp_path = Path(tmp)
            nm = tmp_path / "fake-nm"
            nm.write_text(f"#!/bin/sh\ncat <<'EOF'\n{nm_output}\nEOF\nexit {nm_exit}\n", encoding="utf-8")
            nm.chmod(0o755)
            target = tmp_path / "libx.so"
            if file_exists:
                target.write_bytes(b"")
            return subprocess.run([str(self.SCRIPT), str(nm), str(target), "x"], capture_output=True, text=True, check=False)

    def test_an_emulated_variable_of_q15t_fails(self):
        done = self.run_script("0000000000014f40 d __emutls_v._ZN4q15t12_GLOBAL__N_19t_allowedE\n0000000000001000 t main")
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertIn("__emutls_v._ZN4q15t12_GLOBAL__N_19t_allowedE", done.stdout)

    def test_every_emulated_variable_is_named(self):
        done = self.run_script("0 d __emutls_v.a\n0 d __emutls_v.b")
        self.assertEqual(done.returncode, 1)
        self.assertIn("__emutls_v.a", done.stdout)
        self.assertIn("__emutls_v.b", done.stdout)

    def test_libcxxabis_own_slot_is_allowed(self):
        done = self.run_script("0000000000007b5c0 d __emutls_v._ZZN10__cxxabiv112_GLOBAL__N_19__globalsEvE10eh_globals")
        self.assertEqual(done.returncode, 0, done.stdout)

    def test_another_variable_of_the_cxxabiv1_namespace_fails(self):
        # only libc++abi's eh_globals is allowed, by its exact symbol: a name that merely contains __cxxabiv1 is a variable like any other
        done = self.run_script("0000000000000000 D __emutls_v._ZN10__cxxabiv116t_not_eh_globalsE\n0 t main")
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertIn("__emutls_v._ZN10__cxxabiv116t_not_eh_globalsE", done.stdout)

    def test_a_file_with_no_symbols_is_a_failure(self):
        # a stripped file lists nothing, so nothing was checked
        for listing in ("", "libx.so: no symbols"):
            with self.subTest(listing=listing):
                done = self.run_script(listing)
                self.assertEqual(done.returncode, 1, done.stdout)
                self.assertIn("no symbols", done.stdout)

    def test_an_object_that_only_refers_to_symbols_is_checked(self):
        # undefined symbols are listed with two fields; they count as listed, and an emulated variable among them is not a definition
        done = self.run_script("                 U memcpy")
        self.assertEqual(done.returncode, 0, done.stdout)

    def test_the_runtime_support_functions_are_not_a_variable_of_q15t(self):
        done = self.run_script("000000000006b00c t __emutls_get_address\n0000000000088ac4 b emutls_pthread_key")
        self.assertEqual(done.returncode, 0, done.stdout)

    def test_an_nm_that_fails_is_a_failure(self):
        done = self.run_script("", nm_exit=1)
        self.assertEqual(done.returncode, 1, done.stdout)

    def test_a_missing_file_is_a_failure(self):
        done = self.run_script("", file_exists=False)
        self.assertEqual(done.returncode, 1, done.stdout)


class NdkChecksAreRegistered(unittest.TestCase):
    def test_a_static_initializer_test_per_checked_module(self):
        found = set()
        for path in (Q15T / "mod").glob("*.cmake"):
            for call in re.findall(r"q15t_arm64_object\(([^)]*)\)", path.read_text(encoding="utf-8")):
                found.update(call.split())
        self.assertEqual(found, set(ARM64_OBJECTS))

    def test_an_emulated_tls_test_per_arm64_object(self):
        # q15t_arm64_object registers it beside the static-initializer test, so every object listed above has one
        text = (Q15T / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("NAME ${Q15T_MODULE}.arm64_no_emutls_${m}", text)
        self.assertIn("check/no_emutls.sh", text)
        self.assertIn("llvm-nm", text)

    def test_the_thunk_and_qemu_checks_are_registered(self):
        for relative, names in NDK_CHECKS.items():
            text = (Q15T / relative).read_text(encoding="utf-8")
            for name in names:
                self.assertIn(f"NAME {name}", text)

    def test_the_ndk_preset_chains_the_ndk_toolchain_file(self):
        import json

        presets = json.loads((Q15T / "CMakePresets.json").read_text(encoding="utf-8"))
        ndk = next(p for p in presets["configurePresets"] if p["name"] == "q15t-ndk-checks")
        self.assertEqual(ndk["toolchainFile"], "$env{Q15T_NDK}/build/cmake/android.toolchain.cmake")
        self.assertEqual(ndk["cacheVariables"]["ANDROID_PLATFORM"], "android-26")
        self.assertEqual(ndk["cacheVariables"]["ANDROID_ABI"], "arm64-v8a")


if __name__ == "__main__":
    unittest.main()
