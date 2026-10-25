"""#479: the pnsrad diagnostic hooks are installed from the frame tick, never from the loader's DLL
notification or the LoadLibrary return that applies the pnsrad patches.  MinHook suspends every other
thread while it installs; doing that inside a LdrDllNotification callback (which runs with the loader
lock held) can deadlock the process.

src/runtime/tests/test_pnsrad_diag_queue.cpp pins the queue's behaviour.  These are source-order
sensors for the call sites the queue cannot see: the pnsrad apply step only records the module base,
and the tick is what drains it.  None of them can be driven without pnsrad.dll loading through the
real loader, so the source is what is checked.  tools/tests/test_pnsrad_load_boundary.py pins that the
notification itself calls nothing but the load record (#504)."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function


ROOT = Path(__file__).resolve().parents[2]
ENABLER = ROOT / "src/runtime/patch/pnsrad_enabler.cpp"
TICK = ROOT / "src/runtime/frame/tick.cpp"
INSTALLERS = ("InstallPartyBroadcasterDiag", "InstallPartySendInviteDiag")
HOOK_CALLS = r"\bPatchDetour\s*\(|\bMH_CreateHook\s*\(|\bMH_EnableHook\s*\(|\bHooking::Attach\s*\("


def notification_body() -> str:
    return extract_braced_function(
        ENABLER.read_text(),
        "static void CALLBACK OnDllLoaded(ULONG reason, const LDR_DLL_NOTIFICATION_DATA* data, void*)",
    )


def apply_body() -> str:
    return extract_braced_function(ENABLER.read_text(), "static void ApplyPnsradLoad(uintptr_t base)")


def drain_body() -> str:
    return extract_braced_function(ENABLER.read_text(), "void nevr_pnsrad_enabler::InstallPendingDiagnostics()")


class PnsradDiagDeferral(unittest.TestCase):
    def test_the_pnsrad_apply_step_only_records_the_module_base(self):
        body = apply_body()
        self.assertIn("s_pendingDiagnostics.NoteLoaded(", body, "the apply step no longer queues the diagnostics")
        for site, text in (("the pnsrad apply step", body), ("the DLL notification", notification_body())):
            for installer in INSTALLERS:
                self.assertNotIn(installer + "(", text, f"{installer} runs inside {site}")
            self.assertNotRegex(text, HOOK_CALLS, f"a hook is installed inside {site}")

    def test_only_the_drain_calls_the_installers(self):
        source = ENABLER.read_text()
        drain = drain_body()
        for installer in INSTALLERS:
            calls = [m.start() for m in re.finditer(r"\b" + installer + r"\s*\(", source)]
            definition = source.index("static void " + installer + "(") + len("static void ")
            self.assertEqual(len(calls), 2, f"{installer}: expected the definition and one call")
            self.assertIn(definition, calls)
            self.assertIn(installer + "(base)", drain, f"the drain does not call {installer}")

    def test_only_the_tick_drains_the_diagnostics(self):
        source = ENABLER.read_text()
        self.assertEqual(source.count("s_pendingDiagnostics.Drain("), 1)
        self.assertIn("s_pendingDiagnostics.Drain(", drain_body())
        calls = re.findall(r"\bInstallPendingDiagnostics\s*\(", source)
        self.assertEqual(len(calls), 1, "InstallPendingDiagnostics is called inside the enabler (only the tick calls it)")

    def test_the_tick_drains_the_queue(self):
        body = extract_braced_function(TICK.read_text(), "void DispatchPerFrameWork(uint64_t nowUs)")
        self.assertIn("nevr_pnsrad_enabler::InstallPendingDiagnostics()", body)
        self.assertIn('#include "runtime/patch/pnsrad_enabler.h"', TICK.read_text())


if __name__ == "__main__":
    unittest.main()
