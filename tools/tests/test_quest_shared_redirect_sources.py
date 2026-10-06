#!/usr/bin/env python3
"""Host-side guard that PCVR and Quest compile the same portable sources."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def cmake_block(text: str, opener: str, closer: str) -> str:
    start = text.find(opener)
    if start < 0:
        raise AssertionError(f"missing CMake block: {opener}")
    end = text.find(closer, start)
    if end < 0:
        raise AssertionError(f"unterminated CMake block: {opener}")
    return text[start:end]


def target_link_libraries(text: str, target: str) -> set[str]:
    match = re.search(rf"target_link_libraries\s*\(\s*{re.escape(target)}\b([^)]*)\)", text, re.DOTALL)
    if match is None:
        return set()
    return set(re.findall(r"[A-Za-z0-9_:+./-]+", match.group(1)))


class SharedRedirectSourceParity(unittest.TestCase):
    def test_production_and_test_targets_use_the_shared_source_and_vectors(self) -> None:
        runtime = (ROOT / "src/runtime/CMakeLists.txt").read_text()
        quest = (ROOT / "src/quest/CMakeLists.txt").read_text()
        runtime_test = (ROOT / "src/runtime/tests/test_service_map.cpp").read_text()
        quest_test = (ROOT / "src/quest/tests/service_redirect_test.cpp").read_text()
        runtime_pool_test = (ROOT / "src/runtime/tests/test_stable_string_pool.cpp").read_text()
        runtime_pool_fixture = (ROOT / "src/runtime/tests/stable_string_pool_fixture.cpp").read_text()
        runtime_accessor_test = (ROOT / "src/runtime/tests/test_service_config.cpp").read_text()
        quest_pool_test = (ROOT / "src/quest/tests/stable_string_pool_test.cpp").read_text()
        runtime_config = (ROOT / "src/runtime/lifecycle/config.cpp").read_text()
        runtime_config_test = (ROOT / "src/runtime/tests/test_service_config.cpp").read_text()

        patches = cmake_block(runtime, "set(PATCHES_SOURCES", "set(PATCHES_HEADERS")
        headers = cmake_block(runtime, "set(PATCHES_HEADERS", "add_library(nevr_runtime SHARED")
        service_test = cmake_block(runtime, "add_executable(test_service_map", "gtest_discover_tests(test_service_map")
        quest_lib = cmake_block(quest, "add_library(nevr_quest_service_redirect", "# Android-linkable test executable")

        self.assertIn('"lifecycle/service_redirect.cpp"', patches)
        self.assertIn("lifecycle/service_redirect.cpp", service_test)
        self.assertIn("../runtime/lifecycle/service_redirect.cpp", quest_lib)
        self.assertIn("nevr_quest_service_redirect", target_link_libraries(quest, "service_redirect_test"))
        self.assertIn('"quest/tests/service_redirect_vectors.h"', runtime_test)
        self.assertIn('"quest/tests/service_redirect_vectors.h"', quest_test)

        pool_test = cmake_block(runtime, "add_executable(test_service_config", "gtest_discover_tests(test_service_config")
        self.assertIn('"lifecycle/stable_string_pool.cpp"', patches)
        self.assertIn("tests/test_stable_string_pool.cpp", pool_test)
        self.assertIn("lifecycle/stable_string_pool.cpp", pool_test)
        self.assertIn("lifecycle/service_redirect.cpp", pool_test)
        self.assertIn("../runtime/lifecycle/stable_string_pool.cpp", quest)
        self.assertIn("InternStableCStr", runtime_pool_fixture)
        self.assertIn("DLL_PROCESS_DETACH", runtime_pool_fixture)
        self.assertIn("AdapterProviderDetachLeavesEscapedValueReadable", runtime_accessor_test)
        self.assertIn("stable_string_pool.cpp", cmake_block(runtime, "add_library(test_stable_string_pool_fixture", "add_library(test_stable_string_pool_destructible_control"))
        self.assertIn('"quest/tests/stable_string_pool_vectors.h"', runtime_pool_test)
        self.assertIn('"quest/tests/stable_string_pool_vectors.h"', quest_pool_test)
        self.assertIn("nevr_quest_stable_string_pool", target_link_libraries(quest, "stable_string_pool_test"))
        redirect_adapter = cmake_block(runtime_config, "static CHAR* RedirectServiceUrl(", "CHAR* JsonValueAsStringHook(")
        self.assertIn("ChooseRedirectedOrOriginal(result, redirected)", redirect_adapter)
        self.assertIn("lifecycle/config_redirect_result.h", headers)
        self.assertIn("config_redirect_result.h", runtime_config)
        self.assertIn("ChooseRedirectedOrOriginal(gameResult, nullptr)", runtime_config_test)

    def test_link_assertion_rejects_a_replaced_quest_library(self) -> None:
        quest = (ROOT / "src/quest/CMakeLists.txt").read_text()
        mutated = quest.replace(
            "target_link_libraries(service_redirect_test PRIVATE nevr_quest_service_redirect)",
            "target_link_libraries(service_redirect_test PRIVATE copied_redirect)",
        )
        self.assertNotIn("nevr_quest_service_redirect", target_link_libraries(mutated, "service_redirect_test"))

    def test_pool_link_assertion_rejects_a_replaced_quest_library(self) -> None:
        quest = (ROOT / "src/quest/CMakeLists.txt").read_text()
        mutated = quest.replace(
            "target_link_libraries(stable_string_pool_test PRIVATE nevr_quest_stable_string_pool)",
            "target_link_libraries(stable_string_pool_test PRIVATE copied_pool)",
        )
        self.assertNotIn("nevr_quest_stable_string_pool", target_link_libraries(mutated, "stable_string_pool_test"))


if __name__ == "__main__":
    unittest.main()
