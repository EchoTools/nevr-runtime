"""The -spectatorstream NOP sits inside PreprocessCommandLine itself. Written before the first
original call it puts a server in the spectator flow, and in that flow the game begins multiplayer
by itself (on a server against the production services, about ten seconds after the login), a
second time after the begin the state machine queues (state_machine.cpp, #514): the second begin
rebinds the HTTP API listener and the server exits. So the hook never writes it; the one write is
PatchEnableServer's, in the deferred bootstrap that runs after the original call returns. These
are source-order sensors; the byte logic is covered by src/runtime/tests/test_checked_patch.cpp."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]
ORIGINAL_CALL = "EchoVR::PreprocessCommandLine(pGame)"
ROUTINE = "PatchSpectatorStreamCheck("
ROUTINE_SIGNATURE = "nevr_checked_patch::Result PatchSpectatorStreamCheck("
# Bare names, so a second call in another spelling or through a function pointer is still seen.
DEFERRED_BOOTSTRAP = re.compile(r"\bRunDeferredRuntimeBootstrap\b")
ENABLE_SERVER = re.compile(r"\bPatchEnableServer\b")
# The site's RVA as a literal (addresses.h holds the one definition).
SITE_RVA = re.compile(r"\b(?:0x0*116f3d|1142589)\b", re.IGNORECASE)
# Ways to write game memory; the hook uses none of them.
WRITE_PRIMITIVES = re.compile(r"\b(?:ApplyPatch|ProcessMemcpy|nevr_checked_patch::Apply|memcpy|WriteProcessMemory|VirtualProtect)\b")
# A routine that builds its own {expected, patched} pair, in any initialization form.
OWN_SITE = re.compile(r"\bSite\s*\{|\bSite\s+\w+\s*(?:=\s*)?[{(]")
# The record's result argument: the last argument of FormatSpectatorStreamRecord must be `result`.
RECORD_RESULT = re.compile(r"FormatSpectatorStreamRecord\([^;]*,\s*result\s*\)\s*;")
# The bits the spectator-stream path sets in game+0x7AE0 (echovr.exe 0x140116F4D), as a hex literal or as
# bit 25 (0x2000000) shifted into place. Scoped to boot.cpp: elsewhere 0x2000000 is also the fixed-time-step
# flag at another offset (server_timestep.h) and an image-size bound (crash_recovery.cpp).
SPECTATOR_BITS = re.compile(r"\b0x0*2[01]00000\b|<<\s*25\b", re.IGNORECASE)


def runtime_sources():
    """The shipped C++ sources under src/, comments stripped: no tests."""
    for path in sorted((ROOT / "src").rglob("*.cpp")):
        parts = path.relative_to(ROOT / "src").parts
        if "tests" in parts:
            continue
        yield path.relative_to(ROOT).as_posix(), strip_comments(path.read_text(errors="replace"))


def without_strings(code: str) -> str:
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', code)


def hook_body() -> str:
    source = (ROOT / "src/runtime/lifecycle/boot.cpp").read_text()
    return extract_braced_function(source, "UINT64 PreprocessCommandLineHook(PVOID pGame)")


class SpectatorStreamOrder(unittest.TestCase):
    def test_the_hook_never_writes_the_spectator_stream_site(self):
        # Neither the routine nor a write of its own: on a server or a client, before the original
        # call or after it, PreprocessCommandLineHook leaves the site to PatchEnableServer.
        body = hook_body()
        self.assertIn(ORIGINAL_CALL, body)
        self.assertNotIn(ROUTINE, body, "PreprocessCommandLineHook applies the spectator-stream patch itself")
        self.assertNotIn("SPECTATORSTREAM_CHECK", body, "PreprocessCommandLineHook writes the spectator-stream site")
        self.assertNotRegex(body, ENABLE_SERVER, "PreprocessCommandLineHook reaches PatchEnableServer (and with it the write) itself")
        self.assertNotRegex(body, WRITE_PRIMITIVES, "PreprocessCommandLineHook writes game memory itself")
        self.assertNotRegex(body, SITE_RVA, "PreprocessCommandLineHook names the spectator-stream site by its RVA")

    def test_patchenableserver_is_called_only_from_the_deferred_bootstrap(self):
        # Every use of the name, not only `PatchEnableServer();`: a function pointer to it is a caller too.
        definition = "VOID PatchEnableServer() {"
        users = []
        for path, code in runtime_sources():
            code = without_strings(code)  # the phase string "PatchEnableServer" is not a use
            users.append((path, len(ENABLE_SERVER.findall(code)) - code.count(definition)))
        self.assertEqual([u for u in users if u[1]], [("src/runtime/lifecycle/boot.cpp", 1)])
        boot = (ROOT / "src/runtime/lifecycle/boot.cpp").read_text()
        deferred = extract_braced_function(boot, "void RunDeferredRuntimeBootstrap(PVOID pGame, const char* trigger)")
        self.assertEqual(len(re.findall(r"\bPatchEnableServer\s*\(\s*\)\s*;", deferred)), 1)

    def test_the_deferred_bootstrap_is_reached_only_from_its_two_known_sites(self):
        # A call anywhere else (in PreflightRuntimeBootstrap, in a helper the hook calls, in another file) can run
        # first; the bootstrap's one-time latch then turns the hook's call after the original into a no-op, and
        # PatchEnableServer writes the site before the first PreprocessCommandLine. So every use of the name in shipped
        # code is pinned: the definition and the hook's call (boot.cpp), and the client's device hook, which a server
        # never takes (headless_graphics.cpp).
        uses = {path: len(DEFERRED_BOOTSTRAP.findall(without_strings(code))) for path, code in runtime_sources()}
        self.assertEqual(
            {path: n for path, n in uses.items() if n},
            {"src/runtime/lifecycle/boot.cpp": 2, "src/runtime/patch/headless_graphics.cpp": 1},
        )
        graphics = (ROOT / "src/runtime/patch/headless_graphics.cpp").read_text()
        client = extract_braced_function(
            graphics, "static void CompleteClientRuntimeBootstrap(const char* trigger, HRESULT result, void* device)"
        )
        self.assertRegex(client, r"if\s*\(\s*!g_isServer\s*&&[^{]*\{[^{}]*\bRunDeferredRuntimeBootstrap\s*\(")

    def test_the_game_flags_field_is_written_only_from_boot_cpp(self):
        # game+0x7AE0 (GAME_WINDOWED_FLAGS_OFFSET) holds the spectator bits; boot.cpp never sets them
        # (test_boot_never_sets_the_spectator_bits_itself), and no other shipped file addresses the field by name.
        users = [path for path, code in runtime_sources() if "GAME_WINDOWED_FLAGS_OFFSET" in code]
        self.assertEqual(users, ["src/runtime/lifecycle/boot.cpp"])

    def test_only_the_routine_names_the_site_in_shipped_code(self):
        # Any other use of SPECTATORSTREAM_CHECK in a .cpp, in any spelling (ApplyPatch(SPECTATORSTREAM_CHECK, ...),
        # nevr_patch_addresses::SPECTATORSTREAM_CHECK, a pointer to it), is a second writer of the site.
        source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
        routine = extract_braced_function(source, ROUTINE_SIGNATURE)
        users = []
        for path, code in runtime_sources():
            if path == "src/runtime/patch/mode_patches.cpp":
                self.assertEqual(code.count(routine), 1)
                code = code.replace(routine, "")
            if re.search(r"\bSPECTATORSTREAM_CHECK\b", code) or SITE_RVA.search(code):
                users.append(path)
        self.assertEqual(users, [], "the spectator-stream site is named outside PatchSpectatorStreamCheck")

    def test_boot_never_sets_the_spectator_bits_itself(self):
        # The same flow, reached by setting game+0x7AE0's bits directly instead of through the NOP.
        code = strip_comments((ROOT / "src/runtime/lifecycle/boot.cpp").read_text())
        self.assertIsNone(SPECTATOR_BITS.search(code), "boot.cpp sets the spectator-stream bits in game+0x7AE0")

    def test_the_one_write_is_patchenableserver_after_the_original_call(self):
        callers = []
        for path in sorted((ROOT / "src").rglob("*.cpp")):
            code = strip_comments(path.read_text(errors="replace"))
            calls = len(re.findall(r"\b" + re.escape(ROUTINE), code)) - code.count(ROUTINE_SIGNATURE)
            if calls:
                callers.append((path.relative_to(ROOT).as_posix(), calls))
        self.assertEqual(callers, [("src/runtime/patch/mode_patches.cpp", 1)])
        server = extract_braced_function(
            (ROOT / "src/runtime/patch/mode_patches.cpp").read_text(), "VOID PatchEnableServer()"
        )
        self.assertIn(ROUTINE, server)
        boot = (ROOT / "src/runtime/lifecycle/boot.cpp").read_text()
        deferred = extract_braced_function(boot, "void RunDeferredRuntimeBootstrap(PVOID pGame, const char* trigger)")
        self.assertIn("PatchEnableServer()", deferred)
        body = hook_body()
        uses = list(DEFERRED_BOOTSTRAP.finditer(body))
        self.assertEqual(len(uses), 1, "the hook reaches RunDeferredRuntimeBootstrap more than once (a second, earlier call?)")
        self.assertLess(
            body.index(ORIGINAL_CALL),
            uses[0].start(),
            "the deferred bootstrap (and with it the spectator-stream write) runs before the original call",
        )

    def test_spectator_bits_pattern_matches_both_spellings(self):
        for literal in ("0x2100000", "0x02100000", "0x2000000", "0X2100000", "1ull << 25", "(1<<25)"):
            with self.subTest(literal=literal):
                self.assertRegex(literal, SPECTATOR_BITS)
        for literal in ("0x0100000", "0x8000000", "0x21000000", "1ull << 250", "1 << 2"):
            with self.subTest(literal=literal):
                self.assertNotRegex(literal, SPECTATOR_BITS)
        for literal in ("0x116F3D", "0x00116f3d", "1142589"):
            with self.subTest(literal=literal):
                self.assertRegex(literal, SITE_RVA)
        for literal in ("0x116F3E", "11425890"):
            with self.subTest(literal=literal):
                self.assertNotRegex(literal, SITE_RVA)

    def test_patchenableserver_writes_the_site_only_through_the_checked_routine(self):
        source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
        body = extract_braced_function(source, "VOID PatchEnableServer()")
        self.assertNotIn("SPECTATORSTREAM_CHECK", body, "PatchEnableServer writes the spectator-stream site itself")
        self.assertIn(ROUTINE, body)

    def routine_body(self) -> str:
        source = (ROOT / "src/runtime/patch/mode_patches.cpp").read_text()
        return extract_braced_function(source, ROUTINE_SIGNATURE)

    def test_routine_validates_and_records_through_log(self):
        # Log() routes by boot phase itself (#593): through the boot tee while the routine runs under
        # the loader lock at boot, through the game's CLog after. The routine does not branch on it.
        body = strip_comments(self.routine_body())
        self.assertIn("nevr_checked_patch::Apply(", body)
        self.assertRegex(body, r"\bLog\(\s*ok\s*\?\s*EchoVR::LogLevel::Info\s*:\s*EchoVR::LogLevel::Warning\s*,\s*\"%s\"\s*,\s*record\s*\)")
        self.assertNotIn("InBootPhase", body)
        self.assertNotIn("TeeFprintf", body)
        self.assertNotRegex(body, r"\bfprintf\s*\(|\bprintf\s*\(", "PatchSpectatorStreamCheck prints outside Log()")

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
