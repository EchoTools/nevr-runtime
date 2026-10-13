"""#519: the -spectatorstream NOP sits inside PreprocessCommandLine itself, so on a server it
must be written BEFORE the original call returns control to the game, not only from
RunDeferredRuntimeBootstrap (which runs after that call).  These are source-order sensors; the
byte logic is covered by src/runtime/tests/test_checked_patch.cpp."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function


ROOT = Path(__file__).resolve().parents[2]
ORIGINAL_CALL = "EchoVR::PreprocessCommandLine(pGame)"
ROUTINE = "PatchSpectatorStreamCheck("


def hook_body() -> str:
    source = (ROOT / "src/runtime/lifecycle/boot.cpp").read_text()
    return extract_braced_function(source, "UINT64 PreprocessCommandLineHook(PVOID pGame)")


class SpectatorStreamOrder(unittest.TestCase):
    def test_server_applies_the_patch_before_the_original_preprocess_call(self):
        body = hook_body()
        self.assertIn(ORIGINAL_CALL, body)
        self.assertIn(ROUTINE, body, "PreprocessCommandLineHook never applies the spectator-stream patch")
        self.assertLess(
            body.index(ROUTINE),
            body.index(ORIGINAL_CALL),
            "the spectator-stream patch is applied after the original PreprocessCommandLine call",
        )

    def test_patch_before_the_call_is_server_only(self):
        # N146: a client's first original call initializes D3D; nothing client-side may change there.
        body = hook_body()
        match = re.search(r"if\s*\(\s*g_isServer\s*\)\s*\{(?P<then>[^{}]*)\}", body)
        self.assertIsNotNone(match, "no `if (g_isServer) { ... }` block in PreprocessCommandLineHook")
        self.assertIn(ROUTINE, match.group("then"))
        self.assertLess(match.start(), body.index(ORIGINAL_CALL))
        self.assertEqual(body.count(ROUTINE), 1, "the patch must be applied in exactly one place in the hook")
        self.assertNotIn("BeginMultiplayer", body, "no synthesized begin next to the patch (#519)")

    def test_deferred_bootstrap_reaches_the_patch_only_as_a_verified_noop(self):
        source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
        body = extract_braced_function(source, "VOID PatchEnableServer()")
        self.assertNotRegex(
            body,
            r"ApplyPatch\s*\(\s*SPECTATORSTREAM_CHECK",
            "PatchEnableServer still blind-writes the spectator-stream site",
        )
        self.assertIn(ROUTINE, body)

    def test_routine_validates_logs_through_the_boot_tee_and_never_logs_under_the_loader_lock(self):
        source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
        body = extract_braced_function(source, "nevr_checked_patch::Result PatchSpectatorStreamCheck(")
        self.assertIn("nevr_checked_patch::Apply(", body)
        self.assertIn("kSpectatorStreamExpected", body)
        match = re.search(
            r"if\s*\(\s*nevr_boot_log_tee::InBootPhase\(\)\s*\)\s*\{(?P<then>.*?)\}\s*else\s*\{(?P<other>.*?)\}",
            body,
            re.S,
        )
        self.assertIsNotNone(match, "the record must branch on nevr_boot_log_tee::InBootPhase()")
        self.assertIn("nevr_boot_log_tee::TeeFprintf(", match.group("then"))
        self.assertNotRegex(match.group("then").replace("TeeFprintf(", ""), r"\bLog\(")
        self.assertIn("Log(", match.group("other"))
        for key in ("addr=", "expected=", "actual=", "result="):
            self.assertIn(key, match.group("then"), f"record lacks {key}")


if __name__ == "__main__":
    unittest.main()
