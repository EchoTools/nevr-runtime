"""#2050 (ADR 0041): quest_patch_libr15 is built by a CMake target (nevr_quest_patch_libr15, file name unchanged), and the `quest-patcher` recipe builds it through the preset.

The recipe holds no compiler or linker command; the sources, the flags and the include paths of the tool are the target's
(src/quest/host/CMakeLists.txt); the compiler is the preset's (src/quest/CMakePresets.json). The scope is this recipe and
these files only: the host test recipes that still carry compiler lines belong to #2049.
"""

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RECIPE = "quest-patcher"
TARGET = "nevr_quest_patch_libr15"
SOURCES = [
    "../patch/quest_patch_libr15.cpp",
    "../patch/libr15_patcher.cpp",
    "../patch/libr15_extra_rows.cpp",
    "../patch/libr15_patcher_cli.cpp",
    "../patch/libr15_patch_identity.cpp",
    "../data/sha256.cpp",
    "../sentinel/arm64_reloc.cpp",
]

# ADR 0041, "Finding a violation" V1: the compilers and linkers a recipe must not run.
V1 = re.compile(
    r"(^|[^[:alnum:]_.+-])(g\+\+|gcc|clang\+\+|clang|ndk-build|windres|x86_64-w64-mingw32-(gcc|g\+\+|ld|ar|windres)"
    r"|aarch64-linux-android[0-9]*-clang(\+\+)?)($|[^[:alnum:]_+-])".replace("[:alnum:]", "A-Za-z0-9")
)
# `ld` and `ar` are words too, so they count only where a command starts.
LINKER = re.compile(r"(^|[;&|(]\s*)(ld|ar)(\s|$)")


def recipe_text(justfile: str, name: str) -> str:
    start = re.search(r"(?m)^" + re.escape(name) + r"[^\n]*:[^\n]*$", justfile)
    if start is None:
        raise AssertionError(f"recipe {name} not found")
    end = re.search(r"(?m)^(?=[A-Za-z_#])", justfile[start.end() :])
    return justfile[start.start() : start.end() + (end.start() if end else len(justfile))]


def code_lines(text: str):
    """The lines of a recipe that run something: no comment lines."""
    return [line for line in text.splitlines() if not line.lstrip().startswith("#")]


def compiler_commands(recipe: str):
    return [line for line in code_lines(recipe) if V1.search(line) or LINKER.search(line.strip())]


def strip_cmake_comments(text: str) -> str:
    return re.sub(r"(?m)#[^\n]*", "", text)


class QuestPatcherRecipe(unittest.TestCase):
    def setUp(self):
        self.recipe = recipe_text((ROOT / "justfile").read_text(), RECIPE)

    def test_the_recipe_runs_no_compiler_or_linker(self):
        self.assertEqual(compiler_commands(self.recipe), [])

    def test_the_check_catches_the_line_the_recipe_used_to_have(self):
        old = (
            '    g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc -Isdk/include -isystem "$out/json_inc" \\\n'
            "        src/quest/patch/quest_patch_libr15.cpp -o \"$out/quest_patch_libr15\"\n"
        )
        self.assertNotEqual(compiler_commands(self.recipe + old), [])
        self.assertNotEqual(compiler_commands(self.recipe + "    x86_64-w64-mingw32-g++ -c a.cpp\n"), [])
        self.assertNotEqual(compiler_commands(self.recipe + "    ld -o a b.o\n"), [])

    def test_the_recipe_configures_and_builds_the_target_through_the_preset(self):
        self.assertIn("cmake --preset quest-patcher", self.recipe)
        self.assertRegex(self.recipe, r"cmake --build --preset quest-patcher --target " + TARGET + r"\b")

    def test_the_recipe_keeps_its_argument_output_path_and_checks_the_output(self):
        self.assertRegex(self.recipe.split("\n", 1)[0], r'^quest-patcher json_include="": _vcpkg-quest-host$')
        self.assertIn('out="build/quest-patcher"', self.recipe)
        self.assertIn('[ -x "$out/quest_patch_libr15" ]', self.recipe)

    def test_no_recipe_lists_the_tools_main_source(self):
        # The host patcher tests (#2049) still compile the test programs by hand; the tool itself is only built here.
        justfile = (ROOT / "justfile").read_text()
        for line in justfile.splitlines():
            if "quest_patch_libr15.cpp" in line:
                self.fail(f"a recipe lists the tool's main source: {line.strip()}")


class QuestPatcherTarget(unittest.TestCase):
    def setUp(self):
        self.cmake = strip_cmake_comments((ROOT / "src/quest/host/CMakeLists.txt").read_text())

    def test_the_target_has_exactly_the_tools_sources(self):
        m = re.search(r"add_executable\(\s*" + TARGET + r"\s+([^)]*)\)", self.cmake)
        self.assertIsNotNone(m, "add_executable(" + TARGET + " ...) not found")
        self.assertEqual(m.group(1).split(), SOURCES)
        for source in SOURCES:
            self.assertTrue((ROOT / "src/quest/host" / source).is_file(), source)

    def test_the_target_keeps_the_flags_of_the_line_it_replaced(self):
        self.assertRegex(self.cmake, r"target_compile_options\(" + TARGET + r" PRIVATE -O2 -Wall -Wextra -Werror\)")
        self.assertIn("set(CMAKE_CXX_STANDARD 17)", self.cmake)
        self.assertIn("set(CMAKE_CXX_EXTENSIONS OFF)", self.cmake)  # -std=c++17, not gnu++17
        self.assertRegex(self.cmake, r'target_include_directories\(' + TARGET + r' PRIVATE "\$\{NEVR_REPO_ROOT\}/src" "\$\{NEVR_REPO_ROOT\}/sdk/include"\)')
        self.assertRegex(self.cmake, r'target_include_directories\(' + TARGET + r' SYSTEM PRIVATE "\$\{NEVR_HOST_JSON_INCLUDE\}"\)')

    def test_the_output_lands_where_the_repack_recipe_looks(self):
        self.assertIn('RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"', self.cmake)
        self.assertIn("OUTPUT_NAME quest_patch_libr15", self.cmake)
        presets = json.loads((ROOT / "src/quest/CMakePresets.json").read_text())
        configure = {p["name"]: p for p in presets["configurePresets"]}["quest-patcher"]
        self.assertTrue(configure["binaryDir"].endswith("/../../build/quest-patcher"))
        repack = recipe_text((ROOT / "justfile").read_text(), "android-repack-apk")
        self.assertIn('patcher="$root/build/quest-patcher/quest_patch_libr15"', repack)


class QuestPatcherPreset(unittest.TestCase):
    def setUp(self):
        self.presets = json.loads((ROOT / "src/quest/CMakePresets.json").read_text())

    def test_the_configure_preset_names_the_host_compiler_and_the_host_mode(self):
        configure = {p["name"]: p for p in self.presets["configurePresets"]}["quest-patcher"]
        variables = configure["cacheVariables"]
        self.assertEqual(variables["CMAKE_CXX_COMPILER"], "g++")
        self.assertEqual(variables["NEVR_QUEST_HOST_TOOLS"], "ON")
        self.assertNotIn("toolchainFile", configure)  # the NDK's toolchain is the Android presets'
        self.assertNotIn("CMAKE_BUILD_TYPE", variables)  # a build type adds -DNDEBUG, which the line it replaced did not

    def test_the_build_preset_exists(self):
        builds = {p["name"]: p for p in self.presets["buildPresets"]}
        self.assertEqual(builds["quest-patcher"]["configurePreset"], "quest-patcher")

    def test_the_android_project_enters_the_host_directory_only_in_host_mode(self):
        text = strip_cmake_comments((ROOT / "src/quest/CMakeLists.txt").read_text())
        self.assertRegex(text, r'option\(NEVR_QUEST_HOST_TOOLS "[^"]*" OFF\)')
        self.assertRegex(text, r"if\(NEVR_QUEST_HOST_TOOLS\)\s+get_filename_component\(NEVR_REPO_ROOT[^\n]*\n\s+add_subdirectory\(host\)\s+return\(\)\s+endif\(\)")
        self.assertLess(text.index("NEVR_QUEST_HOST_TOOLS)"), text.index("if(NOT ANDROID)"))


if __name__ == "__main__":
    unittest.main()
