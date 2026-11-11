"""#2109: a server's frame wait is wired once. The wait's logic (raw pings answered as they arrive, the game's datagrams
handed back in order, never an early return on any thread, the overrun patch's site and gate) is tested by
src/runtime/tests/test_server_frame_wait.cpp and test_server_frame_wait_socket.cpp; the idle tick's wiring is
tools/tests/test_server_idle_tick_wiring.py. These are source sensors for what the GTests cannot see:
- the two raw-ping symbols are the game's CMatSym hashes of their names, recomputed with the repo's implementation;
- the runtime's Wait hook hands a server's wait to nevr_server_frame_wait::Wait after the zero-wait case and before
  its own timer, and says a fallback wait (NoteFallbackWait);
- boot.cpp installs the frame wait in both modes;
- the hooks go through HandBack, and the wrapper hook never drops the queue;
- every server wait goes through ChooseWaitMode and DeadlineUs;
- the broadcaster hooks have no hook_liveness row (they would warn entered=NO on every client and offline server);
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
LIVENESS = ROOT / "src/runtime/hook/hook_liveness.cpp"
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
    call = re.search(r"if\s*\(\s*g_isServer\s*\)\s*\{\s*if\s*\(\s*nevr_server_frame_wait::Wait\(\s*microseconds\s*\)\s*\)"
                     r"\s*return\s*;\s*nevr_server_frame_wait::NoteFallbackWait\(\s*microseconds\s*\)\s*;\s*\}", body)
    if call is None:
        return ["PrecisionSleepWaitHook does not hand a server's wait to nevr_server_frame_wait::Wait, then say a "
                "fallback wait with NoteFallbackWait"]
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


def hand_back_problems(install: str) -> list[str]:
    """The hooks go through HandBack; the wrapper hook never drops the queue; every wait is chosen and never early."""
    problems = []
    code = strip_comments(install)
    recv = function_body(code, "uint64_t __fastcall RecvHook(")
    if recv is None or "g_handBack.OnReceive(" not in recv:
        problems.append("RecvHook does not hand back through HandBack::OnReceive")
    elif "Clear(" in recv:
        problems.append("RecvHook drops the queue: a call for another socket must pass through and leave it")
    drain = function_body(code, "void __fastcall DrainHook(")
    if drain is None or "g_handBack.OnDrain(" not in drain:
        problems.append("DrainHook does not record the drain through HandBack::OnDrain")
    wait = function_body(code, "bool Wait(int64_t microseconds)")
    if wait is None:
        problems.append("Wait is not found")
    else:
        if "ChooseWaitMode(" not in wait or "WaitMode::kClockOnly" not in wait:
            problems.append("Wait does not choose with ChooseWaitMode, so a wait off the loop thread could fall back")
        if "g_handBack.BeginWait(" not in wait:
            problems.append("Wait does not take its socket from HandBack::BeginWait")
    if len(re.findall(r"DeadlineUs\(", code)) < 2:
        problems.append("both waits (socket and clock-only) must take their deadline from DeadlineUs (the margin)")
    if "nevr_hook_liveness" in code:
        problems.append("server_frame_wait.cpp marks a hook_liveness id (its counts are in its own summary line)")
    return problems


def liveness_problems(liveness: str) -> list[str]:
    code = strip_comments(liveness)
    rows = [n for n in ("CBroadcaster::SendRecvDrain", "udp_recvfrom_sockaddr_storage") if f'"{n}"' in code]
    return [f"hook_liveness has a row for {n}: it warns entered=NO on every client and offline server" for n in rows]


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
        text = BUG_FIXES.read_text()
        broken = text.replace("        if (nevr_server_frame_wait::Wait(microseconds)) return;\n", "")
        self.assertNotEqual(broken, text)
        self.assertTrue(wait_hook_problems(broken))
        unsaid = text.replace("        nevr_server_frame_wait::NoteFallbackWait(microseconds);\n", "")
        self.assertNotEqual(unsaid, text)
        self.assertTrue(wait_hook_problems(unsaid))

    def test_boot_installs_it_once(self):
        self.assertEqual(boot_problems(BOOT.read_text()), [])
        self.assertTrue(boot_problems(BOOT.read_text().replace(
            "nevr_server_frame_wait::Install(g_isServer != FALSE);", "")))

    def test_the_hooks_go_through_hand_back_and_every_wait_is_chosen(self):
        self.assertEqual(hand_back_problems(INSTALL.read_text()), [])

    def test_a_wrapper_hook_that_drops_the_queue_is_caught(self):
        text = INSTALL.read_text()
        broken = text.replace("    if (g_handBack.OnReceive(socket, address, buffer, size, status, &result)) return result;\n",
                              "    if (g_handBack.OnReceive(socket, address, buffer, size, status, &result)) return result;\n"
                              "    g_handBack.Queue().Clear();\n")
        self.assertNotEqual(broken, text)
        self.assertTrue(hand_back_problems(broken))

    def test_a_wait_that_falls_back_off_the_loop_thread_is_caught(self):
        text = INSTALL.read_text()
        broken = text.replace("ChooseWaitMode(g_active.load(std::memory_order_acquire), OnGameLoopThread())",
                              "OnGameLoopThread() ? WaitMode::kServeSocket : WaitMode::kNotOurs")
        self.assertNotEqual(broken, text)
        self.assertTrue(hand_back_problems(broken))

    def test_the_broadcaster_hooks_have_no_liveness_row(self):
        self.assertEqual(liveness_problems(LIVENESS.read_text()), [])
        self.assertTrue(liveness_problems(LIVENESS.read_text().replace(
            "const Entry kEntries[] = {\n",
            'const Entry kEntries[] = {\n    {"CBroadcaster::SendRecvDrain", "server, once a frame", false},\n')))

    def test_the_overrun_patch_is_gated(self):
        self.assertEqual(install_problems(INSTALL.read_text()), [])

    def test_an_ungated_overrun_patch_is_caught(self):
        broken = INSTALL.read_text().replace("MayPatchOverrunDelta(true, g_active.load(std::memory_order_acquire))",
                                             "true")
        self.assertTrue(install_problems(broken))


class TheTestIsBuiltAndRun(unittest.TestCase):
    def test_test_auth_unit_builds_and_runs_the_gtest(self):
        cmake = CMAKE.read_text()
        self.assertIn("add_executable(test_server_frame_wait tests/test_server_frame_wait.cpp "
                      "tests/test_server_frame_wait_socket.cpp)", cmake)
        self.assertIn("gtest_discover_tests(test_server_frame_wait", cmake)
        justfile = JUSTFILE.read_text()
        self.assertRegex(justfile, r"--target test_server_frame_wait\b")
        self.assertRegex(justfile, r"for test_name in [^\n]*\btest_server_frame_wait\b")


if __name__ == "__main__":
    unittest.main()
