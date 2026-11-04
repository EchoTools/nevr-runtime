"""The compile checks and `just test-scripting` build through presets and CMake targets (ADR 0041, #2052).

Three places once ran a compiler themselves or configured CMake with flags on the command line:

- `just test-android` compiled `src/quest/tests/symbol_hash_test.cpp` with the NDK's `clang++ -fsyntax-only`;
- `just test-scripting` ran `cmake -S ... -DCMAKE_TOOLCHAIN_FILE=...` and a `g++` line for the host stubs generator;
- the echo-symbols-sync workflow ran `g++ -fsyntax-only` on `src/runtime/log/symcache_data.cpp`.

ADR 0041 rule 3: a recipe, script or workflow step holds no compiler or linker command, and configures with a preset. The
scan is scoped to these three places; the recipes the other ADR 0041 issues convert have their own checks.
"""
import json
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "echo-symbols-sync.yml"

# ADR 0041, "Finding a violation", V1: a compiler or linker command.
COMPILER = re.compile(
    r"(^|[^A-Za-z0-9_.+-])"
    r"(g\+\+|gcc|clang\+\+|clang|ndk-build|windres|x86_64-w64-mingw32-(gcc|g\+\+|ld|ar|windres)"
    r"|aarch64-linux-android[0-9]*-clang(\+\+)?)"
    r"($|[^A-Za-z0-9_+-])"
)
# V2: a configure with the toolchain or compiler options on the command line, where a preset holds them.
CONFIGURE_BY_FLAGS = re.compile(r"cmake\s+-S\b|-DCMAKE_TOOLCHAIN_FILE|-DCMAKE_BUILD_TYPE|-DCMAKE_CXX_COMPILER|-DNEVR_SCRIPT_VM")


def code_lines(text):
    return [line for line in text.splitlines() if not line.lstrip().startswith("#")]


def recipe_code(name):
    body = just_recipe.body(just_recipe.read_justfile(), name)
    if body is None:
        raise AssertionError(f"no {name} recipe in the justfile")
    return code_lines(body)


def preset_names(path, kind):
    data = json.loads((REPO / path).read_text(encoding="utf-8"))
    return {preset["name"] for preset in data.get(kind, [])}


class NoCompilerInTheseRecipes(unittest.TestCase):
    def scan(self, label, lines):
        hits = [line.strip() for line in lines if COMPILER.search(line)]
        self.assertEqual(hits, [], f"{label} holds a compiler or linker command (ADR 0041 rule 3, V1)")
        flags = [line.strip() for line in lines if CONFIGURE_BY_FLAGS.search(line)]
        self.assertEqual(flags, [], f"{label} configures CMake with a toolchain or options on the command line (V2)")

    def test_test_android(self):
        self.scan("just test-android", recipe_code("test-android"))

    def test_test_scripting(self):
        self.scan("just test-scripting", recipe_code("test-scripting"))

    def test_echo_symbols_sync_workflow(self):
        self.scan(str(WORKFLOW.relative_to(REPO)), code_lines(WORKFLOW.read_text(encoding="utf-8")))


class TheScannerSeesTheOldLines(unittest.TestCase):
    """The scan is only worth something if it matches the lines that were there."""

    def test_old_lines_match(self):
        old = [
            '"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++" -std=c++17 -fsyntax-only x.cpp',
            "g++ -std=c++17 -O1 -Wall -Wextra -Werror -pthread -Isrc -Isdk/include -isystem \"$json\" a.cpp -o b",
            "          g++ -std=c++17 -fsyntax-only -I../../src ../../src/runtime/log/symcache_data.cpp",
        ]
        for line in old:
            self.assertTrue(COMPILER.search(line), line)
        flags = 'cmake -S src/scripting -B "$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-mingw64.cmake" \\'
        self.assertTrue(CONFIGURE_BY_FLAGS.search(flags))

    def test_names_of_a_compiler_in_other_words_do_not_match(self):
        for line in ["cmake --build --preset android-arm64", "x86_64-w64-mingw32 toolchain", "see g++-notes.txt"]:
            self.assertFalse(COMPILER.search(line), line)


class TheChecksAreTargetsOfPresets(unittest.TestCase):
    def test_test_android_builds_the_symbol_hash_target_through_the_quest_preset(self):
        code = "\n".join(recipe_code("test-android"))
        match = re.search(r"cmake --build --preset (\S+) --target (\S+)", code)
        self.assertIsNotNone(match, "test-android does not build a target through a build preset")
        preset, target = match.groups()
        self.assertIn(preset, preset_names("src/quest/CMakePresets.json", "buildPresets"))
        cmake = (REPO / "src/quest/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, rf"add_library\({target} OBJECT EXCLUDE_FROM_ALL tests/symbol_hash_test\.cpp\)")
        self.assertRegex(cmake, rf"target_compile_options\({target} PRIVATE[^)]*-fsyntax-only")

    def test_test_scripting_configures_and_builds_by_preset(self):
        code = "\n".join(recipe_code("test-scripting"))
        configure = preset_names("src/scripting/CMakePresets.json", "configurePresets")
        build = preset_names("src/scripting/CMakePresets.json", "buildPresets")
        for mode in ("shared", "pervm"):
            self.assertIn(f"scripting-mingw-{mode}", configure)
            self.assertIn(f"scripting-mingw-{mode}", build)
        self.assertIn('cmake --preset "scripting-mingw-$mode"', code)
        self.assertIn('cmake --build --preset "scripting-mingw-$mode"', code)
        self.assertIn("cmake --preset script-check", code)
        self.assertIn("cmake --build --preset script-check", code)
        self.assertIn("script-check", preset_names("src/scripting/check/CMakePresets.json", "configurePresets"))
        self.assertIn("script-check", preset_names("src/scripting/check/CMakePresets.json", "buildPresets"))

    def test_the_host_stubs_generator_is_a_target_of_the_check_project(self):
        cmake = (REPO / "src/scripting/check/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_executable(nevr_script_stubs_host_test", cmake)
        code = "\n".join(recipe_code("test-scripting"))
        self.assertIn("nevr_script_stubs_host_test", code)

    def test_the_workflow_builds_the_symcache_object_through_its_preset(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("cmake --preset symcache-check", workflow)
        self.assertIn("cmake --build --preset symcache-check", workflow)
        self.assertIn("symcache-check", preset_names("tools/symcache/check/CMakePresets.json", "configurePresets"))
        self.assertIn("symcache-check", preset_names("tools/symcache/check/CMakePresets.json", "buildPresets"))
        cmake = (REPO / "tools/symcache/check/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("src/runtime/log/symcache_data.cpp", cmake)
        self.assertIn("-fsyntax-only", cmake)


if __name__ == "__main__":
    unittest.main()
