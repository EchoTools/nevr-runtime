import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/runtime/patch/asset_cdn.cpp").read_text()


class TintMapLifetime(unittest.TestCase):
    """#352: Hook_LoadoutResolveDataFromId reads g_tintMap with no lock, so every free of the map
    has to come after the hook is detached, the pointer is nulled, and the reader gate is idle."""

    def test_hook_holds_the_reader_gate_before_it_calls_the_original(self):
        body = extract_braced_function(SOURCE, "void* __fastcall Hook_LoadoutResolveDataFromId(")
        scope = body.index("nevr::ReaderGate::Scope")
        self.assertLess(scope, body.index("g_originalFunc("),
                        "the gate scope must cover the trampoline call, not just the map read")
        self.assertLess(scope, body.index("ReaderGate::Load(g_tintMap"))

    def test_shutdown_detaches_nulls_waits_then_frees(self):
        body = extract_braced_function(SOURCE, "void AssetCDN::Shutdown()")
        detach = body.index("Hooking::Detach(")
        null_ptr = body.index("ReaderGate::Publish<TintMap>(g_tintMap, nullptr")
        wait = body.index("g_tintGate.WaitIdle(")
        release = body.index("ReleaseFetchData();")
        self.assertLess(detach, null_ptr)
        self.assertLess(null_ptr, wait)
        self.assertLess(wait, release)
        self.assertNotIn("g_originalFunc = nullptr", body,
                         "the trampoline pointer must stay valid for a call in the detour prologue")

    def test_republish_waits_for_the_gate_before_deleting_the_old_map(self):
        body = extract_braced_function(SOURCE, "static void BackgroundFetchBody()")
        store = body.index("ReaderGate::Publish(g_tintMap, newTintMap")
        wait = body.index("g_tintGate.WaitIdle(", store)
        delete = body.index("delete old;", store)
        self.assertLess(store, wait)
        self.assertLess(wait, delete)


if __name__ == "__main__":
    unittest.main()
