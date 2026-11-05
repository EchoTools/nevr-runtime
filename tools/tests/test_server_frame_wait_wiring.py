"""#2109: a server's frame wait and idle tick are wired once. The wait's logic (raw pings answered as they arrive, the
game's datagrams handed back in order, never an early return, the overrun patch's site and gate) is tested by
src/runtime/tests/test_server_frame_wait.cpp, and the idle step by src/runtime/tests/test_server_timestep.cpp (its
wiring: tools/tests/test_server_idle_tick_wiring.py). These are source sensors for what neither can see:
- the two raw-ping symbols are the game's CMatSym hashes of their names, recomputed with the repo's implementation;
- the runtime's Wait hook hands a server's wait to nevr_server_frame_wait::Wait after the zero-wait case and before
  its own timer;
- boot.cpp installs the frame wait in both modes;
- Install applies the overrun patch only through MayPatchOverrunDelta;
- test-auth-unit builds and runs the GTest.
Each check takes the text it reads, so a test hands it a broken copy and sees it fail."""
from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/runtime/patch/server_frame_wait.h"
INSTALL = ROOT / "src/runtime/patch/server_frame_wait.cpp"
BUG_FIXES = ROOT / "src/runtime/patch/binary_bug_fixes.cpp"
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
CMAKE = ROOT / "src/runtime/CMakeLists.txt"
JUSTFILE = ROOT / "justfile"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def function_body(code: str, head: str) -> str | None:
    """The braced body of the first function whose text starts with `head`, or None."""
    start = code.find(head)
    if start < 0:
        return None
    open_at = code.find("{", start)
    if open_at < 0:
        return None
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at + 1:i]
    return None


def load_corpus():
    spec = importlib.util.spec_from_file_location("gen_symbol_corpus", ROOT / "tools/gen_symbol_corpus.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def symbol_problems(header: str, cmatsym_hash) -> list[str]:
    problems = []
    for const, name in (("kRawPingRequest", "raw_ping_request"), ("kRawPingAcknowledge", "raw_ping_acknowledge")):
        m = re.search(rf"constexpr\s+uint64_t\s+{const}\s*=\s*(0x[0-9a-fA-F]+)ULL\s*;", header)
        if m is None:
            problems.append(f"{const} is not a uint64_t constant in server_frame_wait.h")
            continue
        want = cmatsym_hash(name)
        if int(m.group(1), 16) != want:
            problems.append(f"{const} = {m.group(1)}, but CMatSym::Hash({name!r}) = 0x{want:016x}")
    return problems


def wait_hook_problems(bug_fixes: str) -> list[str]:
    body = function_body(strip_comments(bug_fixes), "static void __fastcall PrecisionSleepWaitHook(")
    if body is None:
        return ["PrecisionSleepWaitHook not found in binary_bug_fixes.cpp"]
    call = re.search(r"if\s*\(\s*g_isServer\s*&&\s*nevr_server_frame_wait::Wait\(\s*microseconds\s*\)\s*\)\s*return\s*;",
                     body)
    if call is None:
        return ["PrecisionSleepWaitHook does not hand a server's wait to nevr_server_frame_wait::Wait"]
    problems = []
    zero = body.find("SwitchToThread()")
    timer = body.find("SetWaitableTimerEx")
    if zero < 0 or zero > call.start():
        problems.append("the server wait is called before the zero-wait case (SwitchToThread)")
    if timer < 0 or timer < call.start():
        problems.append("the hook's own timer wait runs before the server wait is offered the call")
    return problems


def boot_problems(boot: str) -> list[str]:
    code = strip_comments(boot)
    n = code.count("nevr_server_frame_wait::Install(g_isServer != FALSE);")
    return [] if n == 1 else [f"boot.cpp calls nevr_server_frame_wait::Install {n} times; it must be once"]


def install_problems(install: str) -> list[str]:
    body = function_body(strip_comments(install), "void Install(bool isServer)")
    if body is None:
        return ["Install not found in server_frame_wait.cpp"]
    problems = []
    gate = body.find("MayPatchOverrunDelta(")
    apply = body.find("nevr_checked_patch::Apply(site, OverrunDeltaSite()")
    if apply < 0:
        problems.append("Install does not apply the overrun patch through nevr_checked_patch with OverrunDeltaSite")
    elif gate < 0 or gate > apply:
        problems.append("the overrun patch is applied without MayPatchOverrunDelta deciding first")
    if "nevr_binary_bug_fixes::FrameWaitOwned()" not in body:
        problems.append("Install does not ask whether the runtime owns CPrecisionSleep::Wait")
    return problems


class ServerFrameWaitWiring(unittest.TestCase):
    def test_the_ping_symbols_are_the_games_hashes(self):
        corpus = load_corpus()
        self.assertEqual(symbol_problems(HEADER.read_text(), corpus.cmatsym_hash), [])

    def test_a_wrong_symbol_is_caught(self):
        corpus = load_corpus()
        broken = HEADER.read_text().replace("0x997279de065a03b0ULL", "0x997279de065a03b1ULL")
        self.assertTrue(symbol_problems(broken, corpus.cmatsym_hash))

    def test_the_wait_hook_hands_a_server_wait_over(self):
        self.assertEqual(wait_hook_problems(BUG_FIXES.read_text()), [])

    def test_a_wait_hook_without_the_call_is_caught(self):
        broken = BUG_FIXES.read_text().replace(
            "if (g_isServer && nevr_server_frame_wait::Wait(microseconds)) return;", "")
        self.assertTrue(wait_hook_problems(broken))

    def test_boot_installs_it_once(self):
        self.assertEqual(boot_problems(BOOT.read_text()), [])
        self.assertTrue(boot_problems(BOOT.read_text().replace(
            "nevr_server_frame_wait::Install(g_isServer != FALSE);", "")))

    def test_the_overrun_patch_is_gated(self):
        self.assertEqual(install_problems(INSTALL.read_text()), [])

    def test_an_ungated_overrun_patch_is_caught(self):
        broken = INSTALL.read_text().replace("MayPatchOverrunDelta(true, g_active.load(std::memory_order_acquire))",
                                             "true")
        self.assertTrue(install_problems(broken))


class TheTestIsBuiltAndRun(unittest.TestCase):
    def test_test_auth_unit_builds_and_runs_the_gtest(self):
        cmake = CMAKE.read_text()
        self.assertIn("add_executable(test_server_frame_wait tests/test_server_frame_wait.cpp)", cmake)
        self.assertIn("gtest_discover_tests(test_server_frame_wait", cmake)
        justfile = JUSTFILE.read_text()
        self.assertRegex(justfile, r"--target test_server_frame_wait\b")
        self.assertRegex(justfile, r"for test_name in [^\n]*\btest_server_frame_wait\b")


if __name__ == "__main__":
    unittest.main()
