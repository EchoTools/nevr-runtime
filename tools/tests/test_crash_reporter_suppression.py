import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function


ROOT = Path(__file__).resolve().parents[2]


class CrashReporterSuppression(unittest.TestCase):
    def test_both_createprocess_hooks_arm_exit_suppression(self):
        """#25 gap 1: a blocked BsSndRpt launch through CreateProcessA arms
        g_crashReporterSuppressed exactly as the CreateProcessW path does, so the
        ExitProcess/TerminateProcess suppression that follows does not depend on
        which entry point the reporter used."""
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        for signature in ("BOOL WINAPI CreateProcessAHook(", "BOOL WINAPI CreateProcessWHook("):
            body = extract_braced_function(source, signature)
            blocked = len(re.findall(r"crash reporter launch blocked", body))
            armed = len(re.findall(r"g_crashReporterSuppressed\s*=\s*true", body))
            self.assertEqual(blocked, 2, f"{signature}: expected two blocking branches")
            self.assertEqual(armed, blocked, f"{signature}: every blocking branch must arm the flag")


if __name__ == "__main__":
    unittest.main()
