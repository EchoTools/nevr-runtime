"""#1420: the netgun bullet effects are registered once, on a client only, and the build compiles the code the tests
drive. The rewrites are tested by src/runtime/tests/test_netgun_bullets.cpp and the transforms through the resource
seam by src/runtime/tests/test_resource_override.cpp (NetgunBulletTransforms); these are source-order sensors for what
neither can see: the call in boot.cpp, its mode gate and the log for the other mode, and the three build entries. Each
check takes the text it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
RUNTIME_CMAKE = ROOT / "src/runtime/CMakeLists.txt"

CALL = "nevr_netgun_bullets::Install();"
SERVER_LOG = "[NEVR.RESOURCE] netgun bullet effects skipped reason=server_mode"
REGISTRY = "InstallResourceOverride();"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def gated_call_problems(boot: str) -> list[str]:
    """The Install call is in the `if (!g_isServer) {` branch of one if/else, the else logs the skip, and the call comes
    after the registry exists (InstallResourceOverride)."""
    code = strip_comments(boot)
    if code.count(CALL) != 1:
        return [f"expected exactly one `{CALL}` in boot.cpp, found {code.count(CALL)}"]
    start = code.index(CALL)
    if REGISTRY not in code[:start]:
        return [f"`{CALL}` runs before `{REGISTRY}`: the transforms would have no registry to join"]
    depth = 0
    open_at = -1
    for i in range(start - 1, -1, -1):
        if code[i] == "}":
            depth += 1
        elif code[i] == "{":
            if depth == 0:
                open_at = i
                break
            depth -= 1
    if open_at < 0 or not code[:open_at].rstrip().endswith("if (!g_isServer)"):
        return ["the Install call is not directly inside an `if (!g_isServer) {` block"]
    after = code[start + len(CALL):]
    close = re.match(r"\s*\}\s*else\s*\{(.*?)\}", after, flags=re.S)
    if not close:
        return ["the `if (!g_isServer)` block has no else: the server-mode decision is not logged (N122/N124)"]
    if SERVER_LOG not in close.group(1):
        return [f"the else branch does not log `{SERVER_LOG}`"]
    if not re.search(r'Log\(\s*EchoVR::LogLevel::Info\s*,\s*"' + re.escape(SERVER_LOG), close.group(1)):
        return [f"`{SERVER_LOG}` is not logged at Info, the level production keeps (N122/N124)"]
    return []


def target_sources(code: str, target: str) -> str:
    match = re.search(r"add_executable\(" + re.escape(target) + r"\b(.*?)\)", code, flags=re.S)
    return match.group(1) if match else ""


def cmake_problems(cmake: str) -> list[str]:
    code = "\n".join(l.split("#", 1)[0] for l in cmake.splitlines())
    problems = []
    if not re.search(r'"patch/netgun_bullets\.cpp"', code):
        problems.append("patch/netgun_bullets.cpp is not in the runtime sources")
    if "tests/test_netgun_bullets.cpp" not in target_sources(code, "test_crash_recovery"):
        problems.append("tests/test_netgun_bullets.cpp is not built into test_crash_recovery")
    if "patch/netgun_bullets.cpp" not in target_sources(code, "test_behavioral"):
        problems.append("test_behavioral does not link patch/netgun_bullets.cpp: its transform tests drive nothing")
    return problems


class NetgunBulletWiring(unittest.TestCase):
    def test_the_install_is_called_once_on_a_client_and_the_server_skip_is_logged(self):
        self.assertEqual(gated_call_problems(BOOT.read_text(encoding="utf-8")), [])

    def test_the_gate_check_fails_when_the_call_is_not_gated(self):
        boot = BOOT.read_text(encoding="utf-8")
        ungated = boot.replace("  if (!g_isServer) {\n    " + CALL, "  {\n    " + CALL)
        self.assertNotEqual(ungated, boot)
        self.assertTrue(gated_call_problems(ungated))

    def test_the_gate_check_fails_when_the_server_branch_does_not_log(self):
        boot = BOOT.read_text(encoding="utf-8")
        quiet = boot.replace(SERVER_LOG, "[NEVR.RESOURCE] something else")
        self.assertNotEqual(quiet, boot)
        self.assertTrue(gated_call_problems(quiet))

    def test_the_gate_check_fails_when_the_server_skip_is_not_at_info(self):
        boot = BOOT.read_text(encoding="utf-8")
        at = boot.index(SERVER_LOG)
        head = boot.rindex("EchoVR::LogLevel::Info", 0, at)
        for level in ("Debug", "Warning"):
            moved = boot[:head] + "EchoVR::LogLevel::" + level + boot[head + len("EchoVR::LogLevel::Info"):]
            self.assertNotEqual(moved, boot)
            self.assertTrue(gated_call_problems(moved), level)

    def test_the_gate_check_fails_when_the_call_is_gone_doubled_or_only_in_a_comment(self):
        boot = BOOT.read_text(encoding="utf-8")
        self.assertTrue(gated_call_problems(boot.replace(CALL, "")))
        self.assertTrue(gated_call_problems(boot + "\n" + CALL + "\n"))
        self.assertTrue(gated_call_problems(boot.replace(CALL, "// " + CALL)))

    def test_the_gate_check_fails_when_the_call_comes_before_the_registry(self):
        boot = BOOT.read_text(encoding="utf-8")
        early = boot.replace(REGISTRY, "", 1).replace(CALL, CALL + "\n  " + REGISTRY, 1)
        self.assertNotEqual(early, boot)
        self.assertTrue(gated_call_problems(early))

    def test_the_build_compiles_the_transform_and_both_test_files_drive_it(self):
        self.assertEqual(cmake_problems(RUNTIME_CMAKE.read_text(encoding="utf-8")), [])

    def test_the_build_check_fails_when_an_entry_is_gone(self):
        cmake = RUNTIME_CMAKE.read_text(encoding="utf-8")
        for broken in (
            cmake.replace('"patch/netgun_bullets.cpp"', ""),
            cmake.replace("tests/test_netgun_bullets.cpp", ""),
            cmake.replace("      patch/netgun_bullets.cpp\n", ""),
        ):
            self.assertNotEqual(broken, cmake)
            self.assertTrue(cmake_problems(broken))


if __name__ == "__main__":
    unittest.main()
