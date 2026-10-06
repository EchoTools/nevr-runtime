#!/usr/bin/env python3
"""Host-side guard that PCVR and Quest compile the same portable sources."""

from pathlib import Path
import os
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CANONICAL = ROOT / "src/runtime/lifecycle/service_redirect.cpp"
CANONICAL_HEADER = ROOT / "src/runtime/lifecycle/service_redirect.h"
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl", ".inc"}
NOT_A_RETURN_TYPE = {"return", "else", "case", "co_return", "throw", "new", "delete", "sizeof"}


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


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def exported_functions(header: Path) -> set[str]:
    """Names of the functions the shared header declares."""
    names: set[str] = set()
    text = re.sub(r"^\s*#.*$", "", strip_comments(header.read_text()), flags=re.MULTILINE)
    text = re.sub(r"\bnamespace\s+\w+\s*\{", "", text)
    for statement in text.split(";"):
        match = re.search(r"\b([A-Za-z_]\w*)\s*\(", statement)
        if match is not None:
            names.add(match.group(1))
    return names


def matching_paren_end(text: str, open_index: int) -> int:
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    return -1


def defines_function(text: str, name: str) -> bool:
    """True when `text` (comments already stripped) defines `name` with a body.

    A definition has a return type before the name and a `{` after the
    parameter list; a call has neither, and a declaration ends in `;`.
    """
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", text):
        before = text[: match.start()].rstrip()
        while before.endswith("::"):
            before = re.sub(r"[A-Za-z_]\w*\s*::$", "", before).rstrip()
        if not before or not (before[-1].isalnum() or before[-1] in "_>&*"):
            continue
        previous_word = re.search(r"([A-Za-z_]\w*)\W*$", before)
        if previous_word is not None and previous_word.group(1) in NOT_A_RETURN_TYPE:
            continue
        end = matching_paren_end(text, match.end() - 1)
        if end < 0:
            continue
        tail = text[end:]
        if re.match(r"\s*(const\s*)?(noexcept\s*)?\{", tail):
            return True
    return False


def redirect_definitions_outside_canonical() -> list[str]:
    names = exported_functions(CANONICAL_HEADER)
    offenders: list[str] = []
    for tree in ("src/quest", "src/runtime"):
        for path in sorted((ROOT / tree).rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES or path == CANONICAL:
                continue
            text = strip_comments(path.read_text(errors="replace"))
            offenders.extend(
                f"{path.relative_to(ROOT)}: {name}" for name in sorted(names) if defines_function(text, name)
            )
    return offenders


def listed_sources(block: str, cmake_dir: Path) -> set[Path]:
    """Every .cpp token in a CMake block, resolved against the CMake file's directory."""
    resolved: set[Path] = set()
    for token in re.findall(r"[^\s()\"]+\.cpp", block):
        token = token.replace("${CMAKE_CURRENT_SOURCE_DIR}", str(cmake_dir))
        resolved.add(Path(os.path.normpath(cmake_dir / token)))
    return resolved


class SharedRedirectSourceParity(unittest.TestCase):
    def test_production_and_test_targets_use_the_shared_source_and_vectors(self) -> None:
        runtime = (ROOT / "src/runtime/CMakeLists.txt").read_text()
        quest = (ROOT / "src/quest/CMakeLists.txt").read_text()
        runtime_test = (ROOT / "src/runtime/tests/test_service_map.cpp").read_text()
        quest_test = (ROOT / "src/quest/tests/service_redirect_test.cpp").read_text()
        runtime_pool_test = (ROOT / "src/runtime/tests/test_stable_string_pool.cpp").read_text()
        runtime_pool_fixture = (ROOT / "src/runtime/tests/stable_string_pool_fixture.cpp").read_text()
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
        self.assertIn("StableStringPoolFixtureObserve", runtime_pool_fixture)
        self.assertIn("publishedBlockFreed", runtime_pool_test)
        self.assertIn("stable_string_pool.cpp", cmake_block(runtime, "add_library(test_stable_string_pool_fixture", "add_library(test_stable_string_pool_destructible_control"))
        self.assertIn('"quest/tests/stable_string_pool_vectors.h"', runtime_pool_test)
        self.assertIn('"quest/tests/stable_string_pool_vectors.h"', quest_pool_test)
        self.assertIn("nevr_quest_stable_string_pool", target_link_libraries(quest, "stable_string_pool_test"))
        redirect_adapter = cmake_block(runtime_config, "static CHAR* RedirectServiceUrl(", "CHAR* JsonValueAsStringHook(")
        self.assertIn("ChooseRedirectedOrOriginal(result, redirected)", redirect_adapter)
        self.assertIn("lifecycle/config_redirect_result.h", headers)
        self.assertIn("config_redirect_result.h", runtime_config)
        self.assertIn("ChooseRedirectedOrOriginal(gameResult, nullptr)", runtime_config_test)

    def test_no_second_definition_of_the_exported_redirect_functions(self) -> None:
        names = exported_functions(CANONICAL_HEADER)
        self.assertIn("ResolveRedirect", names)
        self.assertEqual(redirect_definitions_outside_canonical(), [])
        self.assertTrue(defines_function(strip_comments(CANONICAL.read_text()), "ResolveRedirect"))

    def test_definition_scan_flags_a_copy_and_ignores_calls_and_declarations(self) -> None:
        copy = "std::optional<std::string> ResolveRedirect(const std::string& r) { return r; }"
        qualified = "std::optional<std::string> nevr_cfg::ResolveRedirect(int a)\n{\n return {}; }"
        call = "if (ResolveRedirect(x)) { use(); }\n auto y = ResolveRedirect(z);\n return ResolveRedirect(w);"
        declaration = "std::optional<std::string> ResolveRedirect(const std::string& r);"
        self.assertTrue(defines_function(copy, "ResolveRedirect"))
        self.assertTrue(defines_function(qualified, "ResolveRedirect"))
        self.assertFalse(defines_function(call, "ResolveRedirect"))
        self.assertFalse(defines_function(declaration, "ResolveRedirect"))

    def test_both_targets_list_exactly_the_one_shared_source(self) -> None:
        runtime_dir = ROOT / "src/runtime"
        quest_dir = ROOT / "src/quest"
        runtime = (runtime_dir / "CMakeLists.txt").read_text()
        quest = (quest_dir / "CMakeLists.txt").read_text()

        patches = listed_sources(cmake_block(runtime, "set(PATCHES_SOURCES", "set(PATCHES_HEADERS"), runtime_dir)
        service_test = listed_sources(
            cmake_block(runtime, "add_executable(test_service_map", "gtest_discover_tests(test_service_map"),
            runtime_dir,
        )
        quest_lib = listed_sources(
            cmake_block(quest, "add_library(nevr_quest_service_redirect", "# Android-linkable test executable"),
            quest_dir,
        )

        for label, sources in (("runtime", patches), ("test_service_map", service_test)):
            redirect_sources = {p for p in sources if p.name.startswith("service_redirect")}
            self.assertEqual(redirect_sources, {CANONICAL}, label)
        self.assertEqual(quest_lib, {CANONICAL}, "quest redirect library")

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
