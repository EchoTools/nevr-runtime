"""#877: a client binds XInput to xinput1_4; a server or headless run keeps the not-connected stubs (2fe27303).

The bind is a hook on the game's symbol resolution, so no unit test reaches it. These are source-order sensors for
what the source can show: the gate that picks the client path, the three symbols it covers, the system-directory
load (N75), and the stub that stays for server and headless runs. Each check takes the text it reads, so a test
can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INITIALIZE = ROOT / "src/runtime/lifecycle/initialize.cpp"

GATE = "if (xinputSymbol && !g_isServer && !g_isHeadless) {"
BIND_CALL = "if (void* bound = BindClientXInput(symbol_name)) return traced(bound);"
SYSTEM_DLL = 'std::string(dir) + "\\\\xinput1_4.dll"'
STUB_LOG = "[NEVR.PATCH] xinput stub name=XInputGetState state=not_connected"
SYMBOLS = ("XInputGetState", "XInputSetState", "XInputGetCapabilities")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def gate_problems(source: str) -> list[str]:
    code = strip_comments(source)
    problems = []
    if code.count(GATE) != 1:
        problems.append(f"expected exactly one `{GATE}`, found {code.count(GATE)}")
    if code.count(BIND_CALL) != 1:
        problems.append(f"expected exactly one `{BIND_CALL}`, found {code.count(BIND_CALL)}")
    if GATE in code and BIND_CALL in code and code.index(BIND_CALL) < code.index(GATE):
        problems.append("the bind runs before the client gate")
    for name in SYMBOLS:
        if f'strcmp(symbol_name, "{name}") == 0' not in code:
            problems.append(f"the xinputSymbol check does not name {name}")
    return problems


def bind_problems(source: str) -> list[str]:
    code = strip_comments(source)
    problems = []
    if SYSTEM_DLL not in code:
        problems.append("xinput1_4.dll is not loaded by its system-directory path (N75)")
    if "GetSystemDirectoryA(" not in code:
        problems.append("no GetSystemDirectoryA call")
    if "LoadLibraryA(path.c_str())" not in code:
        problems.append("the system path is not what LoadLibraryA loads")
    if "GetProcAddress(xinput, symbol_name)" not in code:
        problems.append("the symbol is not resolved from xinput1_4")
    return problems


def stub_problems(source: str) -> list[str]:
    code = strip_comments(source)
    problems = []
    if STUB_LOG not in source:
        problems.append("the not-connected stub log line is gone")
    if "return 0x48F;" not in code:
        problems.append("the not-connected stub return value is gone")
    return problems


class XInputBindingWiring(unittest.TestCase):
    def setUp(self) -> None:
        self.source = INITIALIZE.read_text(encoding="utf-8")

    def test_client_gate_and_bind_call_are_wired_in_the_symbol_hook(self) -> None:
        self.assertEqual(gate_problems(self.source), [])

    def test_bind_loads_xinput1_4_from_the_system_directory(self) -> None:
        self.assertEqual(bind_problems(self.source), [])

    def test_stubs_remain_for_server_and_headless_runs(self) -> None:
        self.assertEqual(stub_problems(self.source), [])

    def test_gate_problem_is_detected_on_a_broken_copy(self) -> None:
        broken = self.source.replace(GATE, "if (xinputSymbol) {")
        self.assertTrue(gate_problems(broken))

    def test_bind_problem_is_detected_on_a_broken_copy(self) -> None:
        broken = self.source.replace(SYSTEM_DLL, '"xinput1_4.dll"')
        self.assertTrue(bind_problems(broken))


if __name__ == "__main__":
    unittest.main()
