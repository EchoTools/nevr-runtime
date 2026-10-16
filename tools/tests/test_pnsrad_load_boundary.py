"""#504: the pnsrad.dll and pnsradmatchmaking.dll patches are applied outside the loader's DLL notification.

The notification (OnDllLoaded) runs under the loader lock, where calling into another module is unsafe. It only
classifies the module and records its base in nevr_pnsrad_load::PendingLoads. The patches land when the game's
LoadLibraryW returns (the DllLoadHook post-load observer), before the game resolves any export of the module, and
the frame tick applies a record that missed that return. The record/drain contract is pinned by
src/runtime/tests/test_pnsrad_load_queue.cpp and the boundary by test_dll_load_hook's
DllLoadHookPostLoad tests; these sensors pin the call sites those tests cannot reach without the game."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]
ENABLER = ROOT / "src/runtime/patch/pnsrad_enabler.cpp"
DLL_LOAD_HOOK = ROOT / "src/runtime/hook/dll_load_hook.cpp"
TICK = ROOT / "src/runtime/frame/tick.cpp"

# What the notification may call: the inline classifier and the lock-free record. Everything else (a memory
# write, a Log() line, the bridge port, a hook) is a call out of the notification's module or a side effect
# under the loader lock.
NOTIFICATION_ALLOWED_CALLS = {"Classify", "Note", "sizeof", "reinterpret_cast", "static_cast"}
CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:<[^<>()]*>)?\s*\(")
CONTROL = {"if", "for", "while", "switch", "return"}


def enabler() -> str:
    return strip_comments(ENABLER.read_text())


def notification_body() -> str:
    return extract_braced_function(enabler(), "static void CALLBACK OnDllLoaded(")


class PnsradLoadBoundary(unittest.TestCase):
    def test_the_dll_notification_only_records_the_load(self):
        body = notification_body()
        calls = {name for name in CALL.findall(body) if name not in CONTROL}
        self.assertLessEqual(
            calls,
            NOTIFICATION_ALLOWED_CALLS,
            f"the DLL notification calls {sorted(calls - NOTIFICATION_ALLOWED_CALLS)} under the loader lock",
        )
        self.assertIn("s_pendingLoads.Note(", body, "the notification no longer records the load")

    def test_the_patches_are_applied_when_load_library_returns(self):
        source = enabler()
        init = extract_braced_function(source, "void nevr_pnsrad_enabler::Init(")
        self.assertIn("nevr_dll_load_hook::AddPostLoadObserver(&ApplyPendingLoadsAtLibraryReturn)", init)
        self.assertLess(
            init.index("AddPostLoadObserver("),
            init.index("regFn(0, OnDllLoaded"),
            "the notification is registered before the observer that applies its records",
        )
        observer = extract_braced_function(source, "static void ApplyPendingLoadsAtLibraryReturn()")
        self.assertIn("ApplyPendingLoads(ApplyPoint::LibraryReturn)", observer)
        apply = extract_braced_function(source, "static size_t ApplyPendingLoads(ApplyPoint at)")
        self.assertIn("ApplyPnsradLoad(base)", apply)
        self.assertIn("ApplyMatchmakingLoad(base)", apply)
        self.assertIn("std::lock_guard<std::mutex> lock(s_applyMutex)", apply,
                      "a LoadLibrary returning while another thread patches must wait for the patch")

    def test_every_load_library_hook_runs_the_observers_after_the_real_call(self):
        source = strip_comments(DLL_LOAD_HOOK.read_text())
        for variant in ("LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW"):
            body = extract_braced_function(source, f"static HMODULE WINAPI Hooked{variant}(")
            original = body.index(f"g_orig{variant}(")
            self.assertIn("RunPostLoadObservers();", body, f"Hooked{variant} never runs the post-load observers")
            observers = body.index("RunPostLoadObservers();")
            self.assertLess(original, observers, f"Hooked{variant} runs the observers before the real load")
            self.assertLess(observers, body.rindex("return result;"))
        runner = extract_braced_function(source, "static void RunPostLoadObservers()")
        self.assertRegex(runner.lstrip("{ \n"), r"^if \(LoaderLockHeldByThisThread\(\)\) return;",
                         "observers must not run while the calling thread holds the loader lock")

    def test_the_tick_applies_loads_that_missed_their_library_return(self):
        body = extract_braced_function(strip_comments(TICK.read_text()), "void DispatchPerFrameWork(uint64_t nowUs)")
        self.assertIn("nevr_pnsrad_enabler::ApplyLoadsMissedAtLibraryReturn()", body)
        drain = extract_braced_function(enabler(), "void nevr_pnsrad_enabler::ApplyLoadsMissedAtLibraryReturn()")
        self.assertIn("ApplyPendingLoads(ApplyPoint::FrameTick)", drain)

    def test_matchmaking_host_patch_runs_on_every_load_without_a_guard(self):
        # Issue #18: pnsradmatchmaking.dll is unloaded and reloaded mid-session, so every recorded load is
        # patched. pnsrad.dll keeps its once-only guard; the matchmaking apply holds no state.
        body = extract_braced_function(enabler(), "static void ApplyMatchmakingLoad(uintptr_t base)")
        self.assertIn("nevr_matchmaker_host_patch::Apply(", body)
        self.assertIn("s_mmLedger.NoteLoad(result)", body)
        self.assertNotRegex(body, r"\bstatic\b|s_pnsradPatched|\bonce\b|\bdone\b",
                            "a guard around the matchmaking host patch leaves a reloaded image unpatched")


if __name__ == "__main__":
    unittest.main()
