"""PatchDetour's failure report must not call Log() while the boot tee is open (#92).

PatchDetour runs from DllMain's InitializeAfterGameImageGuard, under the loader lock. Once
InitializeFunctionPointers() resolves EchoVR::WriteLog, Log() enters the game logger there, so the
N36 census (which only reads InitializeAfterGameImageGuard's own body) cannot see it. The report goes
through BootLogTee::TeeFprintf while BootLogTee::IsOpen(), and through Log() only in the else branch.
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
PATCHING = REPO / "src/runtime/hook/patching.h"
BOOT_TEE_H = REPO / "src/runtime/log/boot_log_tee.h"
BOOT_TEE_CPP = REPO / "src/runtime/log/boot_log_tee.cpp"


def patch_detour_body(text: str) -> str:
    start = text.index("inline BOOL PatchDetour(")
    depth, i = 0, text.index("{", start)
    begin = i
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[begin : i + 1]
        i += 1


def code_only(text: str) -> str:
    return "\n".join(re.sub(r"//.*", "", line) for line in text.splitlines())


class PatchDetourLogging(unittest.TestCase):
    def setUp(self):
        self.body = code_only(patch_detour_body(PATCHING.read_text()))

    def test_boot_branch_uses_the_tee(self):
        match = re.search(r"if\s*\(\s*BootLogTee::IsOpen\(\)\s*\)\s*\{(?P<then>.*?)\}\s*else\s*\{(?P<other>.*?)\}", self.body, re.S)
        self.assertIsNotNone(match, "PatchDetour must branch on BootLogTee::IsOpen()")
        self.assertIn("BootLogTee::TeeFprintf(", match.group("then"))
        self.assertNotIn("Log(", match.group("then").replace("TeeFprintf(", ""))
        self.assertIn("Log(EchoVR::LogLevel::Warning", match.group("other"))

    def test_no_log_call_outside_the_else_branch(self):
        stripped = re.sub(r"else\s*\{\s*Log\(.*?\);\s*\}", "", self.body, flags=re.S)
        self.assertNotRegex(stripped.replace("TeeFprintf(", ""), r"\bLog\(")

    def test_is_open_is_declared_and_defined(self):
        self.assertIn("bool IsOpen();", BOOT_TEE_H.read_text())
        self.assertIn("bool BootLogTee::IsOpen()", BOOT_TEE_CPP.read_text())


if __name__ == "__main__":
    unittest.main()
