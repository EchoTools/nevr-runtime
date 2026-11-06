"""The Quest host tests are CMake targets, and the `test-quest-*` recipes only configure, build and run them (#2049, ADR 0041).

ADR 0041 (docs/adr/0041-native-code-is-built-by-cmake.md) says a justfile recipe contains no compiler or linker command and
builds native code with `cmake --preset` and `cmake --build --preset`. The 18 recipes below compiled their test programs with
`g++` lines of their own. This test pins the three things that keep them converted:

* no compiler, linker or toolchain-on-the-command-line text in those recipes (the ADR's V1 and V2 searches, scoped to them);
* each recipe configures, builds and runs through its own `quest-host-<group>` preset, and the preset exists in
  src/quest/CMakePresets.json (a configure and a build preset, and a test preset for the groups ctest runs);
* every test program the recipes built is still a target of the project (src/quest/host_tests/groups/*.cmake) in the group of
  the recipe that runs it: deleting one fails here, since `ctest` itself only fails on an empty run.
"""

from __future__ import annotations

import json
import pathlib
import re
import unittest

import just_recipe

REPO = just_recipe.REPO
HOST_TESTS = REPO / "src" / "quest" / "host_tests"

# Recipe -> the preset group it runs.
RECIPE_GROUPS = {
    "test-quest-data": "data",
    "test-quest-data-net": "data-net",
    "test-quest-invite": "invite",
    "test-quest-shared": "shared",
    "test-quest-router": "router",
    "test-quest-invite-transport": "invite-transport",
    "test-quest-tls": "tls",
    "test-quest-hooks": "hooks",
    "test-quest-combat": "combat",
    "test-quest-redirect": "redirect",
    "test-quest-hooks-pinned": "hooks-pinned",
    "test-quest-patcher": "patcher",
    "test-quest-patcher-pinned": "patcher-pinned",
    "test-quest-obb": "obb",
    "test-quest-obb-pinned": "obb-pinned",
    "test-quest-social": "social",
    "test-quest-social-pinned": "social-pinned",
    "test-quest-integration": "integration",
    "test-quest-plugins": "plugins",
    "test-quest-autopilot": "autopilot",
    "test-quest-integration-pinned": "integration-pinned",
}

# Groups whose test programs need a harness the recipe starts (servers, certificates) or the store files it extracts: the
# recipe runs them by name, so they are targets and not ctest tests.
RUN_BY_NAME = {"data-net", "tls", "hooks-pinned", "patcher-pinned", "social-pinned", "integration-pinned", "obb-pinned"}

# The test programs each recipe built, as the recipes built them before they were targets.
PROGRAMS = {
    "autopilot": (
        "autopilot_route_test",
        "autopilot_run_test",
    ),
    "combat": (
        "combat_active_test",
        "combat_install_test",
        "combat_level_test",
        "combat_tick_test",
        "equip_refire_test",
        "heap_step_test",
        "heaps_test",
        "match_tag_default_test",
        "memsys_test",
        "refresh_rate_test",
        "pfx_test",
        "pfxtev_test",
        "sendguard_test",
    ),
    "data": (
        "data_step_test",
        "data_test",
        "fetch_test",
        "pack_test",
    ),
    "data-net": (
        "curl_transport_test",
        "data_wiring_test",
    ),
    "hooks": (
        "arm64_reloc_test",
        "build_variant_test",
        "clock_levels_hook_test",
        "crash_first_hook_test",
        "credits_text_test",
        "embedding_check",
        "frame_dispatch_test",
        "got_hook_test",
        "hand_flip_test",
        "hand_pose_log_hook_test",
        "hwdump_share_test",
        "input_seam_test",
        "invite_join_quest_test",
        "irradiance_default_test",
        "libr15_attach_test",
        "libr15_extra_rows_identity_test",
        "lobby_full_hooks_test",
        "lobby_full_test",
        "login_fallback_test",
        "login_prompt_hook_test",
        "login_signin_code_chain_test",
        "login_title_hook_test",
        "menu_cards_hook_test",
        "menu_version_label_test",
        "obb_skip_hook_test",
        "os_build_gate_hook_test",
        "path_rewrite_test",
        "permission_wait_hook_test",
        "pns_provider_hook_test",
        "process_end_hook_test",
        "quit_hook_test",
        "refresh_eye_size_test",
        "render_config_hook_test",
        "resource_request_hook_test",
        "row_hooks_install_test",
        "script_redirect_test",
        "texture_menu_hook_test",
        "texture_menu_test",
        "texture_quality_hook_test",
        "unparsable_identity_test",
        "vk_alloc_check_test",
        "vk_ext_window_test",
        "vk_free_guard_test",
        "wizard_metric_hook_test",
    ),
    "hooks-jni": (
        "permission_request_test",
        "toast_presenter_test",
    ),
    "hooks-pinned": (
        "got_pinned_test",
    ),
    "obb": (
        "fat16_test",
        "obb_cipher_test",
        "obb_footer_test",
        "obb_key_test",
        "stock_data_flow_test",
        "stock_pins_test",
    ),
    "obb-pinned": (
        "obb_pinned_test",
    ),
    "integration": (
        "integration_hooks_test",
        "integration_sequence_test",
        "session_bridge_test",
    ),
    "integration-pinned": (
        "integration_pinned_test",
    ),
    "invite": (
        "invite_service_link_test",
    ),
    "invite-transport": (
        "invite_transport_quest_test",
    ),
    "patcher": (
        "libr15_patch_table_test",
        "libr15_patcher_test",
        "stub_codec_test",
    ),
    "patcher-pinned": (
        "libr15_patcher_pinned_test",
    ),
    "plugins": (
        "libr15_attach_step_test",
        "packaged_plugins_test",
        "plugin_loader_test",
        "plugin_runtime_test",
    ),
    "redirect": (
        "redirect_test",
        "redirect_test_tsan",
    ),
    "router": (
        "loopback_game_server_test",
        "loopback_game_server_test_tsan",
        "remote_ws_test",
        "session_router_test",
        "session_router_test_checked",
        "ws_wire_test",
    ),
    "shared": (
        "auth_core_test",
        "crash_report_quest_test",
        "crash_report_test",
        "elf_build_id_test",
        "evr_codec_test",
        "hwdump_core_test",
        "hwdump_core_test_tsan",
        "hwdump_handlers_test",
        "login_prerequisites_install_test",
        "login_prerequisites_test",
        "login_rewrite_test",
        "login_standin_test",
        "nevr_config_set_test",
        "quest_config_test",
        "sentinel_host_test",
        "service_redirect_test",
        "symbol_hash_test",
        "symbol_hash_test_uchar",
        "tls_ca_test",
    ),
    "social": (
        "social_abi_test",
        "social_facade_test",
        "social_install_test",
        "social_names_test",
    ),
    "social-pinned": (
        "social_pinned_test",
    ),
    "tls": (
        "curl_ws_tls_test",
    ),
}

# ADR 0041, "Finding a violation", V1: a compiler or linker command.
COMPILER = re.compile(
    r"(^|[^A-Za-z0-9_.+-])(g\+\+|gcc|clang\+\+|clang|ndk-build|windres|x86_64-w64-mingw32-(gcc|g\+\+|ld|ar|windres)"
    r"|aarch64-linux-android[0-9]*-clang(\+\+)?)($|[^A-Za-z0-9_+-])",
    re.M,
)


def code_lines(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def calls(text: str, function: str) -> list[str]:
    """The argument text of every `function(...)` call in CMake text, with the parentheses balanced."""
    found = []
    for match in re.finditer(r"\b" + re.escape(function) + r"\(", text):
        depth, i = 1, match.end()
        while depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        found.append(text[match.end() : i - 1])
    return found


def targets() -> dict[str, dict[str, bool]]:
    """group -> {program: runs by name (NO_TEST)} from the nevr_ht_test calls of the project."""
    found: dict[str, dict[str, bool]] = {}
    for path in sorted((HOST_TESTS / "groups").glob("*.cmake")):
        for call in calls(code_lines(path.read_text(encoding="utf-8")), "nevr_ht_test"):
            words = call.split()
            group = words[words.index("GROUP") + 1]
            # A program defined in two branches (a test only when its inputs exist) is a test when any branch registers it.
            found.setdefault(group, {})[words[0]] = found.get(group, {}).get(words[0], True) and "NO_TEST" in words
    return found


class RecipesBuildThroughPresets(unittest.TestCase):
    def setUp(self):
        self.justfile = just_recipe.read_justfile()
        self.presets = json.loads((REPO / "src" / "quest" / "CMakePresets.json").read_text(encoding="utf-8"))

    def recipe(self, name: str) -> str:
        text = just_recipe.recipe(self.justfile, name)
        self.assertIsNotNone(text, name)
        return code_lines(text)

    def test_no_recipe_holds_a_compiler_or_linker_command(self):
        for name in RECIPE_GROUPS:
            with self.subTest(recipe=name):
                hit = COMPILER.search(self.recipe(name))
                self.assertIsNone(hit, f"{name} has a compiler command: {hit and hit.group(0)!r}")

    def test_no_recipe_configures_cmake_without_a_preset(self):
        for name in RECIPE_GROUPS:
            with self.subTest(recipe=name):
                body = self.recipe(name)
                self.assertNotRegex(body, r"cmake\s+-S\b", name)
                self.assertNotRegex(body, r"cmake\s+.*-B\s", name)
                self.assertNotIn("CMAKE_TOOLCHAIN_FILE", body, name)

    def test_each_recipe_configures_and_builds_its_own_preset(self):
        configure = {p["name"] for p in self.presets["configurePresets"]}
        build = {p["name"] for p in self.presets["buildPresets"]}
        test = {p["name"] for p in self.presets.get("testPresets", [])}
        for name, group in RECIPE_GROUPS.items():
            preset = f"quest-host-{group}"
            with self.subTest(recipe=name):
                body = self.recipe(name)
                self.assertIn(f"cmake --preset {preset}", body)
                self.assertIn(f"cmake --build --preset {preset}", body)
                self.assertIn(preset, configure)
                self.assertIn(preset, build)
                if group in RUN_BY_NAME:
                    self.assertNotIn(f"ctest --preset {preset}", body)
                else:
                    self.assertIn(f"ctest --preset {preset}", body)
                    self.assertIn(preset, test)

    def test_the_build_preset_builds_the_group_target_and_the_test_preset_runs_its_label(self):
        build = {p["name"]: p for p in self.presets["buildPresets"]}
        test = {p["name"]: p for p in self.presets.get("testPresets", [])}
        for group in RECIPE_GROUPS.values():
            with self.subTest(group=group):
                self.assertEqual(build[f"quest-host-{group}"]["targets"], [f"host-tests-{group}"])
                if group not in RUN_BY_NAME:
                    self.assertEqual(test[f"quest-host-{group}"]["filter"]["include"]["label"], f"^{group}$")

    def test_the_extra_rows_configure_step_has_a_preset_of_its_own(self):
        path = REPO / "src" / "quest" / "tests" / "extra_rows_configure" / "CMakePresets.json"
        presets = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual([p["name"] for p in presets["configurePresets"]], ["quest-host-extra-rows-configure"])
        self.assertIn("cmake --preset quest-host-extra-rows-configure", self.recipe("test-quest-hooks"))


class EveryProgramIsATarget(unittest.TestCase):
    def test_every_program_the_recipes_built_is_a_target_of_the_project(self):
        found = targets()
        for group, expected in PROGRAMS.items():
            with self.subTest(group=group):
                self.assertEqual(sorted(expected), sorted(found.get(group, {})))

    def test_the_programs_a_recipe_runs_by_name_are_not_ctest_tests_and_the_rest_are(self):
        found = targets()
        for group, programs in found.items():
            base = group.removesuffix("-jni")
            for program, by_name in programs.items():
                with self.subTest(program=program):
                    self.assertEqual(by_name, base in RUN_BY_NAME or program == "unparsable_identity_test"
                                     or program == "embedding_check", program)

    def test_every_group_has_a_group_file_and_a_group_target(self):
        text = "\n".join(p.read_text(encoding="utf-8") for p in (HOST_TESTS / "groups").glob("*.cmake"))
        for group in set(RECIPE_GROUPS.values()):
            with self.subTest(group=group):
                self.assertIn(f"nevr_ht_group({group})", text)


class TheHostToolsProjectIsReused(unittest.TestCase):
    """The host tests ride the host tools project of the quest-patcher preset (#2050); no second entry gate."""

    def test_the_host_presets_turn_on_the_host_tools_and_the_tests(self):
        presets = json.loads((REPO / "src" / "quest" / "CMakePresets.json").read_text(encoding="utf-8"))
        base = {p["name"]: p for p in presets["configurePresets"]}["quest-host-base"]
        self.assertEqual(base["cacheVariables"]["NEVR_QUEST_HOST_TOOLS"], "ON")
        self.assertEqual(base["cacheVariables"]["NEVR_QUEST_HOST_TESTS"], "ON")
        self.assertEqual(base["cacheVariables"]["CMAKE_CXX_COMPILER"], "g++")
        self.assertNotIn("toolchainFile", base)

    def test_the_tests_are_entered_from_the_host_directory(self):
        host = code_lines((REPO / "src" / "quest" / "host" / "CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertRegex(host, r"if\(NEVR_QUEST_HOST_TESTS\)\s+add_subdirectory\([^)]*host_tests")
        top = code_lines((REPO / "src" / "quest" / "CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertNotIn("add_subdirectory(host_tests", top)

    def test_the_extra_rows_check_is_a_host_target_built_through_the_preset(self):
        cmake = code_lines((REPO / "src" / "quest" / "cmake" / "libr15_extra_rows.cmake").read_text(encoding="utf-8"))
        self.assertIsNone(COMPILER.search(cmake), "libr15_extra_rows.cmake names a compiler")
        self.assertNotIn("find_program(NEVR_HOST_CXX", cmake)
        self.assertIn("--preset quest-patcher", cmake)
        self.assertIn("--target nevr_quest_libr15_extra_rows_check", cmake)
        host = code_lines((REPO / "src" / "quest" / "host" / "CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertRegex(host, r"add_executable\(nevr_quest_libr15_extra_rows_check\s+\.\./patch/libr15_extra_rows_check\.cpp")


if __name__ == "__main__":
    unittest.main()
