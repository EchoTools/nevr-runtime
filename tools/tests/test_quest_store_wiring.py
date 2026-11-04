"""#2081 (docs/adr/0042): the store-variant recipes, presets, targets and manifests fit together.

What this reads is the tree: the recipes in `justfile`, the presets in `src/quest/CMakePresets.json`, the targets in
`src/quest/host/CMakeLists.txt` and `src/quest/host_tests/groups/obb.cmake`, the host dependency manifest, the pin file and the
ADR. The behaviour itself is in `test_quest_store_build.py`, `test_quest_store_install.py` and `just test-quest-obb`.
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JUSTFILE = (ROOT / "justfile").read_text()
HOST_CMAKE = (ROOT / "src/quest/host/CMakeLists.txt").read_text()
PRESETS = json.loads((ROOT / "src/quest/CMakePresets.json").read_text())

# ADR 0041, "Finding a violation" V1: a compiler or linker command.
V1 = re.compile(
    r"(^|[^A-Za-z0-9_.+-])(g\+\+|gcc|clang\+\+|clang|ndk-build|windres|x86_64-w64-mingw32-(gcc|g\+\+|ld|ar|windres)"
    r"|aarch64-linux-android[0-9]*-clang(\+\+)?)($|[^A-Za-z0-9_+-])"
)


def recipe(name: str) -> str:
    start = re.search(r"(?m)^" + re.escape(name) + r"[^\n]*:[^\n]*$", JUSTFILE)
    if start is None:
        raise AssertionError(f"recipe {name} not found")
    end = re.search(r"(?m)^(?=[A-Za-z_#\[])", JUSTFILE[start.end():])
    return JUSTFILE[start.start(): start.end() + (end.start() if end else len(JUSTFILE))]


def preset(kind: str, name: str) -> dict:
    found = [p for p in PRESETS[kind] if p["name"] == name]
    if not found:
        raise AssertionError(f"{kind} {name} not found")
    return found[0]


class RecipesTest(unittest.TestCase):
    def test_the_recipes_exist_with_the_dependencies_the_adr_names(self):
        self.assertRegex(recipe("quest-stock-data"), r'(?m)^quest-stock-data: \(_quest-store-require "obb"\) _vcpkg-quest-host$')
        self.assertRegex(recipe("quest-store-build"),
                         r'(?m)^quest-store-build: \(_quest-store-require "apk" "obb" "keystore"\) quest-stock-data build-android quest-patcher$')
        self.assertRegex(recipe("_quest-store-require"), r"quest_store_build\.py require")

    def test_the_stock_data_recipe_builds_through_the_preset_and_runs_the_tool_with_the_pins(self):
        text = recipe("quest-stock-data")
        self.assertIn("cmake --preset quest-obb-extract && cmake --build --preset quest-obb-extract", text)
        self.assertIn('build/quest-obb-extract/quest_obb_extract --obb "$NEVR_STORE_OBB" --pins tools/quest_stock_data.pin --cache-root build/quest-stock-data', text)

    def test_the_store_build_recipe_chains_the_repack_on_the_store_apk_into_build_quest_store(self):
        text = recipe("quest-store-build")
        self.assertIn('just android-repack-apk "$NEVR_STORE_APK" build/android-arm64/sentinel/libovrplatformloader.so build/quest-store/repack', text)
        self.assertLess(text.index("quest_store_build.py check"), text.index("android-repack-apk"))
        self.assertLess(text.index("android-repack-apk"), text.index("quest_store_build.py collect"))
        self.assertIn('[ "$status" -eq 3 ] || exit "$status"', text)  # only a miss rebuilds; any other failure stops

    def test_the_install_recipe_runs_the_install_tool_and_is_not_a_dependency_of_the_build(self):
        self.assertIn('python3 -I tools/quest_store_install.py "$@"', recipe("quest-store-install"))
        self.assertNotIn("quest-store-install", recipe("quest-store-build").split("\n", 1)[0])

    def test_no_recipe_holds_a_compiler_or_linker_command(self):
        for name in ("quest-stock-data", "quest-store-build", "quest-store-install", "_quest-store-require", "test-quest-obb", "test-quest-obb-pinned"):
            with self.subTest(name):
                code = "\n".join(l for l in recipe(name).splitlines() if not l.lstrip().startswith("#"))
                self.assertIsNone(V1.search(code), f"{name} names a compiler (ADR 0041 rule 3)")

    def test_the_inputs_are_named_variables_with_no_machine_path(self):
        for name in ("quest-stock-data", "quest-store-build", "quest-store-install", "_quest-store-require", "test-quest-obb-pinned"):
            text = recipe(name)
            self.assertNotRegex(text, r"/home/|/mnt/|~/", name)
        self.assertIn("NEVR_STORE_OBB", recipe("test-quest-obb-pinned"))
        self.assertIn("NEVR_STORE_OBB names no store OBB", recipe("test-quest-obb-pinned"))

    def test_the_obb_unit_tests_are_part_of_the_verify_parallel_block(self):
        self.assertRegex(JUSTFILE, r"(?m)^\s+test-quest-obb \\$")


class CMakeTest(unittest.TestCase):
    def test_the_presets_exist(self):
        configure = preset("configurePresets", "quest-obb-extract")
        self.assertEqual(configure["cacheVariables"]["NEVR_QUEST_OBB_EXTRACT"], "ON")
        self.assertEqual(configure["cacheVariables"]["CMAKE_CXX_COMPILER"], "g++")
        self.assertEqual(preset("buildPresets", "quest-obb-extract")["targets"], ["nevr_quest_obb_extract"])
        for name in ("quest-host-obb", "quest-host-obb-pinned"):
            self.assertEqual(preset("configurePresets", name)["inherits"], "quest-host-base")
        self.assertEqual(preset("configurePresets", "quest-host-base")["cacheVariables"]["NEVR_QUEST_OBB_EXTRACT"], "ON")
        self.assertEqual(preset("buildPresets", "quest-host-obb")["targets"], ["host-tests-obb"])
        self.assertEqual(preset("buildPresets", "quest-host-obb-pinned")["targets"], ["host-tests-obb-pinned"])
        self.assertEqual(preset("testPresets", "quest-host-obb")["filter"]["include"]["label"], "^obb$")

    def test_the_library_lists_exactly_the_sources_of_src_quest_obb(self):
        listed = set(re.findall(r"\.\./obb/([a-z0-9_]+\.cpp)", HOST_CMAKE))
        on_disk = {p.name for p in (ROOT / "src/quest/obb").glob("*.cpp")}
        self.assertEqual(listed, on_disk)  # a source that is not listed is not built; a listed one that is gone fails the configure

    def test_the_tool_target_has_the_file_name_the_recipes_run(self):
        self.assertIn("add_executable(nevr_quest_obb_extract ../obb/obb_extract_main.cpp)", HOST_CMAKE)
        self.assertRegex(HOST_CMAKE, r"nevr_quest_obb_extract PROPERTIES OUTPUT_NAME quest_obb_extract RUNTIME_OUTPUT_DIRECTORY \"\$\{CMAKE_BINARY_DIR\}\"")

    def test_nettle_comes_from_the_host_manifest_and_not_from_the_system(self):
        manifest = json.loads((ROOT / "tools/quest-host-deps/vcpkg.json").read_text())
        self.assertIn("nettle", manifest["dependencies"])
        self.assertIn('find_library(NEVR_NETTLE_LIB NAMES libnettle.a PATHS "${NEVR_HOST_PREFIX}/lib" NO_DEFAULT_PATH)', HOST_CMAKE)

    def test_every_test_program_of_the_obb_group_has_a_source(self):
        group = (ROOT / "src/quest/host_tests/groups/obb.cmake").read_text()
        names = re.findall(r"nevr_ht_test\((\w+) GROUP obb ", group)
        for name in names + ["obb_pinned_test"]:
            self.assertTrue((ROOT / "src/quest/tests" / f"{name}.cpp").is_file(), name)
        self.assertGreaterEqual(len(names), 6)


class PinsAndAdrTest(unittest.TestCase):
    def test_the_adr_exists_and_names_the_inputs_and_the_pin_file(self):
        adr = (ROOT / "docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md").read_text()
        for needle in ("NEVR_STORE_APK", "NEVR_STORE_OBB", "tools/quest_stock_data.pin", "nevr_quest_obb_extract", "0xf85434", "quest-store-install"):
            self.assertIn(needle, adr)

    def test_the_game_key_string_is_the_one_the_sentinel_adr_records_at_the_mount_call(self):
        key = re.search(r'kGameObbKeyString\[\] = "([0-9a-f]+)"', (ROOT / "src/quest/obb/obb_key.h").read_text()).group(1)
        self.assertIn(key, (ROOT / "docs/adr/0007-quest-obb-mount-skip.md").read_text())

    def test_the_log_tag_is_in_the_logging_standard(self):
        self.assertIn("`[NEVR.QUESTBUILD]`", (ROOT / "docs/standards/logging.md").read_text())


if __name__ == "__main__":
    unittest.main()
