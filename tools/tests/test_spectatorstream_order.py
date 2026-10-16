"""#519: the -spectatorstream NOP sits inside PreprocessCommandLine itself, so on a server it
is written BEFORE the original call, not only from RunDeferredRuntimeBootstrap (which runs after
that call), and once.  These are source-order sensors; they pin where the write is made, not
whether it starts the lobby (it does not: the call the state machine queues after the login
does).  The byte logic is covered by src/runtime/tests/test_checked_patch.cpp."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function


ROOT = Path(__file__).resolve().parents[2]
ORIGINAL_CALL = "EchoVR::PreprocessCommandLine(pGame)"
ROUTINE = "PatchSpectatorStreamCheck("
PREFLIGHT = "PreflightRuntimeBootstrap()"
ROUTINE_SIGNATURE = "nevr_checked_patch::Result PatchSpectatorStreamCheck("
# The server guard: `if (g_isServer && !<flag>.exchange(true, ...)) {`.  The flag is a run-once
# latch, so a re-entered hook does not emit a second pre-preprocess record.
SERVER_ONCE_GUARD = r"if\s*\(\s*g_isServer\s*&&\s*!\s*(?P<flag>\w+)\.exchange\(\s*true\b[^)]*\)\s*\)\s*\{"
# A routine that builds its own {expected, patched} pair, in any initialization form.
OWN_SITE = re.compile(r"\bSite\s*\{|\bSite\s+\w+\s*(?:=\s*)?[{(]")
# The record's result argument: the last argument of FormatSpectatorStreamRecord must be `result`.
RECORD_RESULT = re.compile(r"FormatSpectatorStreamRecord\([^;]*,\s*result\s*\)\s*;")


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

    def test_server_applies_the_patch_after_preflight_sets_g_isserver(self):
        # g_isServer is set inside PreflightRuntimeBootstrap(); the `if (g_isServer)` guarding the
        # patch reads it, so the guard must come after the preflight, not just before the call.
        body = hook_body()
        self.assertEqual(body.count(PREFLIGHT), 1)
        self.assertLess(
            body.index(PREFLIGHT),
            body.index(ROUTINE),
            "the spectator-stream patch is applied before PreflightRuntimeBootstrap() has set g_isServer",
        )
        guard = re.search(SERVER_ONCE_GUARD + r"[^{}]*" + re.escape(ROUTINE), body)
        self.assertIsNotNone(guard)
        self.assertLess(body.index(PREFLIGHT), guard.start())

    def test_patch_before_the_call_is_server_only(self):
        # N146: a client's first original call initializes D3D; nothing client-side may change there.
        body = hook_body()
        match = re.search(SERVER_ONCE_GUARD + r"(?P<then>[^{}]*)\}", body)
        self.assertIsNotNone(match, "no `if (g_isServer && !<latch>.exchange(true)) { ... }` block in PreprocessCommandLineHook")
        self.assertIn(ROUTINE, match.group("then"))
        self.assertLess(match.start(), body.index(ORIGINAL_CALL))
        self.assertEqual(body.count(ROUTINE), 1, "the patch must be applied in exactly one place in the hook")
        self.assertNotIn("BeginMultiplayer", body, "no synthesized begin next to the patch (#519)")

    def test_the_pre_preprocess_record_is_emitted_once_per_process(self):
        # The hook is entered per PreprocessCommandLine call; without a latch every entry would
        # write a record (and re-validate the site).
        body = hook_body()
        match = re.search(SERVER_ONCE_GUARD, body)
        self.assertIsNotNone(match, "the server guard has no run-once latch")
        flag = match.group("flag")
        self.assertRegex(
            body,
            r"static\s+std::atomic_bool\s+" + re.escape(flag) + r"\s*\{\s*false\s*\}\s*;",
            "the latch is not a function-static std::atomic_bool starting false",
        )
        self.assertLess(body.index("static std::atomic_bool"), match.start())

    def test_deferred_bootstrap_reaches_the_patch_only_as_a_verified_noop(self):
        source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
        body = extract_braced_function(source, "VOID PatchEnableServer()")
        self.assertNotRegex(
            body,
            r"ApplyPatch\s*\(\s*SPECTATORSTREAM_CHECK",
            "PatchEnableServer still blind-writes the spectator-stream site",
        )
        self.assertIn(ROUTINE, body)

    def routine_body(self) -> str:
        source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
        return extract_braced_function(source, ROUTINE_SIGNATURE)

    def test_routine_validates_logs_through_the_boot_tee_and_never_logs_under_the_loader_lock(self):
        body = self.routine_body()
        self.assertIn("nevr_checked_patch::Apply(", body)
        match = re.search(
            r"if\s*\(\s*nevr_boot_log_tee::InBootPhase\(\)\s*\)\s*\{(?P<then>.*?)\}\s*else\s*\{(?P<other>.*?)\}",
            body,
            re.S,
        )
        self.assertIsNotNone(match, "the record must branch on nevr_boot_log_tee::InBootPhase()")
        self.assertIn("nevr_boot_log_tee::TeeFprintf(", match.group("then"))
        self.assertNotRegex(match.group("then").replace("TeeFprintf(", ""), r"\bLog\(")
        self.assertIn("Log(", match.group("other"))
        # No unguarded Log() anywhere else in the routine: it runs under the loader lock at boot.
        outside = body.replace(match.group(0), "")
        self.assertNotRegex(outside, r"\bLog\s*\(", "PatchSpectatorStreamCheck logs outside the InBootPhase else branch")
        self.assertNotRegex(outside, r"\bfprintf\s*\(|\bprintf\s*\(", "PatchSpectatorStreamCheck prints outside the tee")

    def test_routine_uses_the_tested_site_pair_the_named_rva_and_the_shared_record(self):
        body = self.routine_body()
        self.assertRegex(
            body,
            r"EchoVR::g_GameBaseAddress\)\s*\+\s*nevr_patch_addresses::SPECTATORSTREAM_CHECK\s*;",
            "the patch site is not g_GameBaseAddress + SPECTATORSTREAM_CHECK",
        )
        # The {expected, patched} pair is built once, in checked_patch.h, where the unit tests
        # pin its order; the routine must not assemble a pair of its own.
        self.assertIn("nevr_checked_patch::SpectatorStreamSite()", body)
        self.assertNotRegex(body, OWN_SITE, "routine builds its own Site")
        self.assertIn("nevr_checked_patch::FormatSpectatorStreamRecord(", body)

    def test_own_site_pattern_catches_every_initialization_form(self):
        for form in (
            "nevr_checked_patch::Site spec{a, b, 6};",
            "nevr_checked_patch::Site spec = {a, b, 6};",
            "const nevr_checked_patch::Site spec = {a, b, 6};",
            "nevr_checked_patch::Site spec(a, b, 6);",
            "return nevr_checked_patch::Site{a, b, 6};",
        ):
            with self.subTest(form=form):
                self.assertRegex(form, OWN_SITE)
        shipped = "const nevr_checked_patch::Site spec = nevr_checked_patch::SpectatorStreamSite();"
        self.assertNotRegex(shipped, OWN_SITE)

    def test_the_recorded_result_is_the_result_apply_returned(self):
        # The record is the only evidence of what happened at the site, so its result= field
        # must be the value Apply returned, and the routine must hand that same value back.
        body = self.routine_body()
        self.assertRegex(
            body,
            r"(?:const\s+)?nevr_checked_patch::Result\s+result\s*=\s*nevr_checked_patch::Apply\(",
            "result is not initialised from nevr_checked_patch::Apply",
        )
        self.assertRegex(body, RECORD_RESULT, "the record's result argument is not `result`")
        self.assertEqual(body.count("FormatSpectatorStreamRecord("), 1)
        self.assertRegex(body, r"\breturn\s+result\s*;")
        self.assertRegex(body, r"\bok\s*=\s*result\s*==")

    def test_record_format_has_every_key_and_the_header_site_is_expected_first(self):
        header = (ROOT / "src/runtime/patch/checked_patch.h").read_text()
        record = re.search(r'"(\[NEVR\.PATCH\] spectatorstream_check[^"]*)"', header)
        self.assertIsNotNone(record, "no spectatorstream_check record format in checked_patch.h")
        for key in ("phase=%s", "addr=0x%llx", "expected=%s", "actual=%s", "result=%s"):
            self.assertIn(key, record.group(1))
        site = extract_braced_function(header, "inline Site SpectatorStreamSite()")
        self.assertRegex(
            site,
            r"Site\{\s*kSpectatorStreamExpected\s*,\s*kSpectatorStreamPatched\s*,",
            "Site{expected, patched, size}: the expected bytes must come first",
        )


if __name__ == "__main__":
    unittest.main()
