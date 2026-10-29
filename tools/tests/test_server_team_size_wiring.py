"""#1422: a server's match team size is wired once. The byte logic and the lines Install logs (InstallLines) are
tested by src/runtime/tests/test_server_team_size.cpp and the two sites' bytes against the game by
tests/system/mode_patches_test.go (TestServerTeamSizeSites); these are source sensors for what neither can see:
the one call in boot.cpp, reached in both modes, the config key, the two addresses, the production writer, the level
each line is logged at, and that test-auth-unit builds and runs the GTest. Each check takes the text it reads, so a
test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
INSTALL = ROOT / "src/runtime/patch/server_team_size.cpp"
ADDRESSES = ROOT / "src/runtime/hook/addresses.h"

CALL = "nevr_server_team_size::Install(g_isServer != FALSE);"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


MODE_RETURN = re.compile(r"if\s*\(\s*!?\s*g_isServer\b[^)]*\)\s*(?:\{[^{}]*?)?\breturn\b")
LAMBDA_HEAD = re.compile(r"\]\s*(?:\([^()]*\))?\s*(?:mutable\s*|noexcept\s*|->\s*[\w:<>]+\s*)*$")


def mode_return(code: str) -> bool:
    """A `return` inside a block, or the `else` of an `if`, whose condition names g_isServer, at any depth; a return in
    a lambda's body is the lambda's own and does not count."""
    stack: list[tuple[str, bool]] = []  # (the block's head, is a lambda)
    closed: dict[int, str] = {}  # the head of the block that last closed at each depth, for an `else`
    head_start = 0
    for m in re.finditer(r"[{};]|\breturn\b", code):
        tok = m.group()
        if tok == "{":
            head = code[head_start:m.start()].strip()
            if re.match(r"else\b", head):  # an `else` or `else if` runs only when the `if` before it did not
                head = head + " " + closed.get(len(stack), "")
            stack.append((head, bool(LAMBDA_HEAD.search(head))))
        elif tok == "}":
            head, _ = stack.pop()
            closed[len(stack)] = head
        elif tok == "return" and not any(is_lambda for _, is_lambda in stack):
            if any(re.search(r"\bg_isServer\b", head) for head, _ in stack):
                return True
        head_start = m.end()
    return False


def boot_problems(boot: str) -> list[str]:
    """One call, in RunDeferredRuntimeBootstrap, outside any `if`, with no return on the mode before it, so both
    modes reach it and a client logs."""
    code = strip_comments(boot)
    if code.count(CALL) != 1:
        return [f"expected exactly one `{CALL}` in boot.cpp, found {code.count(CALL)}"]
    start = code.index(CALL)
    body = code.find("void RunDeferredRuntimeBootstrap(")
    if body < 0 or body > start:
        return ["the Install call is not in RunDeferredRuntimeBootstrap"]
    depth = 0
    for ch in code[code.index("{", body):start]:
        depth += ch == "{"
        depth -= ch == "}"
    if depth != 1:
        return [f"the Install call is nested {depth - 1} blocks deep; a client would not log its skip line"]
    before = code[:start].rstrip()
    if not before.endswith((";", "{", "}")):
        return ["the Install call is the body of an `if` or a loop without braces"]
    if MODE_RETURN.search(code[body:start]) or mode_return(code[code.index("{", body) + 1:start]):
        return ["RunDeferredRuntimeBootstrap returns on the mode before the Install call"]
    return []


def install_problems(install: str) -> list[str]:
    code = strip_comments(install)
    problems = []
    if not re.search(r"isServer\s*\?\s*NevrCfgGetFlat\(\s*\"nevr_max_team_size\"\s*\)\s*:\s*nullptr", code):
        problems.append("a server's size is not read from nevr_max_team_size")
    for name in ("SERVER_ASSIGN_TEAM_CAP", "SERVER_ASSIGN_TEAM_SLOT"):
        if not re.search(rf"base\s*\+\s*nevr_patch_addresses::{name}\b", code):
            problems.append(f"{name} is not the site handed to InstallLines")
    if not re.search(r"InstallLines\(\s*isServer\s*,\s*text\s*,\s*capSite\s*,\s*slotSite\s*,\s*ProcessWrite\s*,"
                     r"\s*LastWriteError\s*,"
                     r"\s*reinterpret_cast<uintptr_t>\(\s*capSite\s*\)\s*,\s*reinterpret_cast<uintptr_t>\(\s*slotSite\s*\)\s*\)", code):
        problems.append("InstallLines is not called with the mode, the value, the two sites, ProcessWrite, "
                        "LastWriteError and their addresses")
    if not re.search(r"return\s+g_writeErrors\.Note\(\s*ProcessMemcpy\(\s*dest\s*,\s*src\s*,\s*n\s*,\s*&error\s*\)\s*,"
                     r"\s*error\s*\)", code):
        problems.append("ProcessWrite does not write through ProcessMemcpy and note its error")
    if not re.search(r"uint32_t\s+LastWriteError\(\)\s*\{\s*return\s+g_writeErrors\.last\s*;\s*\}", code):
        problems.append("LastWriteError does not return the error ProcessWrite noted")
    if not re.search(r"Log\(\s*line\.warning\s*\?\s*EchoVR::LogLevel::Warning\s*:\s*EchoVR::LogLevel::Info\s*,\s*\"%s\"\s*,"
                     r"\s*line\.text\.c_str\(\)\s*\)", code):
        problems.append("each line is not logged as text at its own level")
    return problems


def address_problems(addresses: str) -> list[str]:
    code = strip_comments(addresses)
    want = {"SERVER_ASSIGN_TEAM_CAP": "0x1AB8C2", "SERVER_ASSIGN_TEAM_SLOT": "0x1ABC8A",
            "SERVER_ASSIGN_TEAM_CAP_SIZE": "12", "SERVER_ASSIGN_TEAM_SLOT_SIZE": "26"}
    problems = []
    for name, rva in want.items():
        if not re.search(rf"constexpr\s+(?:uintptr_t|size_t)\s+{name}\s*=\s*{rva}\s*;", code):
            problems.append(f"{name} is not {rva} (ServerAssignTeam, echovr.exe 0x140000000 + RVA)")
    return problems


class ServerTeamSizeWiring(unittest.TestCase):
    def test_the_tree(self) -> None:
        self.assertEqual(boot_problems(BOOT.read_text(encoding="utf-8")), [])
        self.assertEqual(install_problems(INSTALL.read_text(encoding="utf-8")), [])
        self.assertEqual(address_problems(ADDRESSES.read_text(encoding="utf-8")), [])

    def test_a_missing_or_doubled_or_gated_call_is_found(self) -> None:
        boot = BOOT.read_text(encoding="utf-8")
        self.assertTrue(boot_problems(boot.replace(CALL, "")))
        self.assertTrue(boot_problems(boot.replace(CALL, CALL + "\n  " + CALL)))
        self.assertTrue(boot_problems(boot.replace(CALL, "if (g_isServer) { " + CALL + " }")))
        self.assertTrue(boot_problems(boot.replace(CALL, "// " + CALL)))
        self.assertTrue(boot_problems(boot.replace(CALL, "if (g_isServer) " + CALL)))
        self.assertTrue(boot_problems(boot.replace(CALL, "if (!g_isServer) return;\n  " + CALL)))
        self.assertTrue(boot_problems(boot.replace(CALL, "if (!g_isServer) {\n    return;\n  }\n  " + CALL)))
        self.assertTrue(boot_problems(boot.replace(CALL, "if (g_isServer) {\n    (void)0;\n  } else {\n    return;\n  }\n  " + CALL)))
        self.assertTrue(boot_problems(boot.replace(
            CALL, "if (!g_isServer) {\n    if (trigger) {\n      (void)0;\n    }\n    return;\n  }\n  " + CALL)))

    def test_a_lambdas_return_or_a_return_on_another_condition_is_not_a_mode_return(self) -> None:
        self.assertFalse(mode_return("if (!g_isServer) {\n  auto f = [] { return 1; };\n  f();\n}\n"))
        self.assertFalse(mode_return("if (!g_isServer) {\n  auto f = [&](int x) -> int { return x; };\n}\n"))
        self.assertFalse(mode_return("if (trigger) {\n  return;\n}\nif (g_isServer) {\n  (void)0;\n}\n"))
        self.assertFalse(mode_return("if (g_isServer) {\n  (void)0;\n}\nif (trigger) {\n  (void)0;\n} else {\n  return;\n}\n"))
        self.assertTrue(mode_return("if (g_isServer) {\n  auto f = [] { return 1; };\n  return;\n}\n"))
        self.assertTrue(mode_return("if (!g_isServer) {\n  if (trigger) {\n    return;\n  }\n}\n"))
        self.assertTrue(mode_return("if (g_isServer) {\n  (void)0;\n} else if (trigger) {\n  return;\n}\n"))
        self.assertTrue(mode_return("if (g_isServer) {\n} else if (a) {\n} else {\n  return;\n}\n"))

    def test_a_broken_install_is_found(self) -> None:
        install = INSTALL.read_text(encoding="utf-8")
        breaks = [
            ('isServer ? NevrCfgGetFlat("nevr_max_team_size") : nullptr', 'NevrCfgGetFlat("nevr_max_team_size")'),
            ('"nevr_max_team_size"', '"nevr_team_size"'),
            ("nevr_patch_addresses::SERVER_ASSIGN_TEAM_SLOT", "nevr_patch_addresses::SERVER_ASSIGN_TEAM_CAP"),
            ("slotSite, ProcessWrite,", "slotSite, nullptr,"),
            ("reinterpret_cast<uintptr_t>(slotSite))", "reinterpret_cast<uintptr_t>(capSite))"),
            ("ProcessMemcpy(dest, src, n, &error)", "ProcessMemcpy(dest, src, n)"),
            ("g_writeErrors.Note(ProcessMemcpy(dest, src, n, &error), error)", "ProcessMemcpy(dest, src, n, &error)"),
            ("return g_writeErrors.last;", "return 0;"),
            ("ProcessWrite, LastWriteError,", "ProcessWrite, nullptr,"),
            ("EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info", "EchoVR::LogLevel::Info : EchoVR::LogLevel::Info"),
        ]
        for old, new in breaks:
            with self.subTest(old=old):
                self.assertIn(old, install)
                self.assertTrue(install_problems(install.replace(old, new)))

    def test_a_moved_address_is_found(self) -> None:
        addresses = ADDRESSES.read_text(encoding="utf-8")
        self.assertTrue(address_problems(addresses.replace("= 0x1AB8C2;", "= 0x1AB8C3;")))
        self.assertTrue(address_problems(addresses.replace("= 0x1ABC8A;", "= 0x1ABC8B;")))
        self.assertTrue(address_problems(addresses.replace("SERVER_ASSIGN_TEAM_SLOT_SIZE = 26;", "SERVER_ASSIGN_TEAM_SLOT_SIZE = 18;")))


class TheTestIsBuiltAndRun(unittest.TestCase):
    """test-auth-unit builds and runs test_server_team_size, so the GTests are part of the quick check."""

    def test_cmake_builds_it(self) -> None:
        cmake = strip_comments((ROOT / "src/runtime/CMakeLists.txt").read_text(encoding="utf-8").replace("#", "//"))
        self.assertIn("add_executable(test_server_team_size tests/test_server_team_size.cpp)", cmake)
        self.assertIn("gtest_discover_tests(test_server_team_size", cmake)

    def test_the_unit_recipe_builds_and_runs_it(self) -> None:
        justfile = (ROOT / "justfile").read_text(encoding="utf-8")
        self.assertRegex(justfile, r"--target test_server_team_size\b")
        self.assertRegex(justfile, r"for test_name in [^\n]*\btest_server_team_size\b")


if __name__ == "__main__":
    unittest.main()
