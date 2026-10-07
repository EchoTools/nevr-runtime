import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def strip_comments(text: str) -> str:
    """Remove // line comments and /* */ block comments from C++ source text.

    A regex over raw source cannot distinguish "the call is here" from "the
    call used to be here and is now commented out" — both contain the same
    substring. Strip comments first so a commented-out call site (with or
    without a decoy real statement nearby) is invisible to the regex, the
    same way the compiler would see it. String/char literals are tracked so
    a literal containing "//" or "/*" is not treated as a comment start.
    Newlines are preserved so line numbers in any future diagnostics stay
    roughly aligned; comment bodies are dropped, not blanked to the same width.
    """
    result = []
    i = 0
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_string = False
    in_char = False
    while i < n:
        c = text[i]
        two = text[i : i + 2]
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
                result.append(c)
            i += 1
            continue
        if in_block_comment:
            if two == "*/":
                in_block_comment = False
                i += 2
                # The compiler treats a block comment as whitespace, so a
                # closed block comment must leave a separator behind it —
                # otherwise "return/**/Unregister();" collapses into
                # "returnUnregister();" and \bUnregister can no longer match
                # a real call that a comment merely interrupts.
                result.append(" ")
            else:
                if c == "\n":
                    result.append("\n")
                i += 1
            continue
        if in_string:
            result.append(c)
            if c == "\\" and i + 1 < n:
                result.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if in_char:
            result.append(c)
            if c == "\\" and i + 1 < n:
                result.append(text[i + 1])
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue
        if two == "//":
            in_line_comment = True
            i += 2
            continue
        if two == "/*":
            in_block_comment = True
            i += 2
            continue
        if c == '"':
            in_string = True
            result.append(c)
            i += 1
            continue
        if c == "'":
            in_char = True
            result.append(c)
            i += 1
            continue
        result.append(c)
        i += 1
    return "".join(result)


def extract_braced_function(source: str, signature: str) -> str:
    """Extract a function body by brace-matching, with comments stripped.

    Every sensor in this file matches against the returned text with regex
    or substring checks, none of which can tell real code from a comment
    describing it. Stripping here, once, means every sensor gets the same
    comment-transparency the compiler has — not just the ones a reviewer
    happened to wrap by hand (#123).
    """
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return strip_comments(source[opening : index + 1])
    raise AssertionError(f"unterminated function body: {signature}")


class RuntimeLifecycleInvariantTest(unittest.TestCase):
    def test_dllmain_does_not_run_plugin_shutdown_or_unload(self):
        source = (ROOT / "src/runtime/lifecycle/dllmain.cpp").read_text()
        body = extract_braced_function(source, "BOOL APIENTRY DllMain(")

        self.assertNotRegex(body, r"\bUnloadPlugins\s*\(")
        self.assertNotIn("NvrPluginShutdown", body)

    def test_dllmain_frees_the_real_dbgcore_only_on_dynamic_unload(self):
        # Issue #40: FreeLibrary(g_realDbgCore) ran on process termination too (lpReserved != NULL).
        # Calling FreeLibrary from DllMain during termination can leave a module in use after the
        # system ran its termination code; only a dynamic unload (lpReserved == NULL) frees it.
        source = (ROOT / "src/runtime/lifecycle/dllmain.cpp").read_text()
        body = extract_braced_function(source, "BOOL APIENTRY DllMain(")

        detach = body.index("case DLL_PROCESS_DETACH")
        section = body[detach:]
        frees = [m.start() for m in re.finditer(r"\bFreeLibrary\s*\(\s*g_realDbgCore\s*\)", body)]
        self.assertEqual(len(frees), 1, "subject vanished: expected exactly one FreeLibrary(g_realDbgCore)")
        free = frees[0] - detach
        self.assertGreater(free, 0, "FreeLibrary(g_realDbgCore) is not in the DLL_PROCESS_DETACH case")
        guard = re.search(r"if\s*\(\s*lpReserved\s*==\s*NULL\s*\)\s*\{", section)
        self.assertIsNotNone(guard, "subject vanished: no lpReserved == NULL guard in DLL_PROCESS_DETACH")
        depth, end = 0, None
        for index in range(guard.end() - 1, len(section)):
            if section[index] == "{":
                depth += 1
            elif section[index] == "}":
                depth -= 1
                if depth == 0:
                    end = index
                    break
        self.assertIsNotNone(end)
        self.assertTrue(guard.end() < free < end,
                        "FreeLibrary(g_realDbgCore) is outside the lpReserved == NULL block")

    def test_both_createprocess_hooks_record_a_blocked_crash_reporter(self):
        # Issue #25: only the wide hook set g_crashReporterSuppressed, so a reporter launched through
        # CreateProcessA was blocked but the later ExitProcess/TerminateProcess suppression logs
        # (which gate on the flag) misattributed the cause.
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        for signature in ("BOOL WINAPI CreateProcessAHook(", "BOOL WINAPI CreateProcessWHook("):
            body = extract_braced_function(source, signature)
            blocks = body.count("return FALSE;")
            self.assertEqual(blocks, 2, f"subject vanished: expected two blocking branches in {signature}")
            self.assertEqual(len(re.findall(r"g_crashReporterSuppressed\s*=\s*true\s*;\s*return\s+FALSE\s*;", body)),
                             blocks, f"a blocking branch of {signature} does not record the suppression")

    def test_the_posix_handler_success_line_is_conditional_on_both_registrations(self):
        # Issue #25: "POSIX signal handlers installed" was logged after the two signal() calls
        # whether or not either returned SIG_ERR.
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        stripped = strip_comments(source)
        self.assertEqual(stripped.count("POSIX signal handlers installed"), 1)
        self.assertRegex(stripped, r"if\s*\(\s*sigintOk\s*&&\s*sigtermOk\s*\)\s*\{\s*Log\([^;]*POSIX signal handlers installed")
        self.assertRegex(stripped, r"const\s+bool\s+sigintOk\s*=\s*signal\(\s*SIGINT")
        self.assertRegex(stripped, r"const\s+bool\s+sigtermOk\s*=\s*signal\(\s*SIGTERM")

    def test_early_boot_never_reads_config(self):
        # service_config.cpp NevrCfg() loads config.yaml on first access and documents that first
        # access is after the CLI is parsed (g_isServer, -config-path). 3d4a994 called
        # NevrCfgSocialFacadeEnabled() from the boot sequence and a real server with a config.yaml
        # died silently right after "minhook initialized".
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")

        self.assertNotRegex(body, r"\bNevrCfg\w*\s*\(")
        self.assertNotRegex(body, r"\bNevrGame\w*\s*\(")

    def test_every_boot_detour_result_is_consumed(self):
        # Issue #42: seven PatchDetour calls in the boot sequence discarded their result, so a
        # hook that never installed could not reach g_bootHookFailed and a server booted as if
        # it had. The boot sequence detours only through InstallBootDetour, which hands the
        # result and the per-hook required/optional decision to NoteBootHookResult.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")

        self.assertNotRegex(body, r"\bPatchDetour\s*\(",
                            "the boot sequence calls PatchDetour directly; use InstallBootDetour")
        self.assertGreater(len(re.findall(r"\bInstallBootDetour\s*\(", body)), 0,
                           "subject vanished: no InstallBootDetour calls found")
        helper = extract_braced_function(source, "static void InstallBootDetour(")
        self.assertRegex(helper, r"NoteBootHookResult\(\s*PatchDetour\(")
        # The two Hooking::Attach results use the same pattern, not a hand-written flag write.
        self.assertNotRegex(body, r"if\s*\(\s*!\s*r\d\s*\)\s*g_bootHookFailed")
        self.assertRegex(body, r"NoteBootHookResult\(\s*r1\s*,[^;]*kRequired")
        self.assertRegex(body, r"NoteBootHookResult\(\s*r2\s*,[^;]*kRequired")
        # A required failure is what sets the flag boot.cpp checks before starting a server.
        note = extract_braced_function(source, "static void NoteBootHookResult(")
        self.assertRegex(note, r"kRequired\)\s*\{\s*g_bootHookFailed\s*=\s*true;")
        # An installed hook (installed == true) must short-circuit before either branch runs;
        # inverting or deleting this line would mark every successful hook as a failure, and
        # every required hook succeeds in the logs this classification is based on, so
        # `just verify` would still go green while every server refused to start.
        self.assertIn("if (installed) return;", note)
        # TeeFprintf, not Log(): this runs under the DllMain loader lock (N36). The N36 census
        # only scans InitializeAfterGameImageGuard's own body and would not catch a Log() call
        # added inside this helper.
        self.assertNotRegex(note, r"\bLog\s*\(")
        # The two Hooking::Attach results (r1, r2) must be captured in a variable, not dropped —
        # a new bare `Hooking::Attach(...)` statement would silently discard its result the same
        # way the original seven PatchDetour calls did.
        attach_calls = re.findall(r"\bHooking::Attach\s*\(", body)
        captured_calls = re.findall(r"\bBOOL\s+r\d+\s*=\s*Hooking::Attach\s*\(", body)
        self.assertEqual(len(attach_calls), len(captured_calls),
                         "a Hooking::Attach call's result is not captured in a variable")

    def test_only_reviewed_boot_hooks_are_optional(self):
        # A required hook that fails makes a server refuse to start (boot.cpp: g_bootHookFailed ->
        # ServerFatal). EchoVR::GetProcAddress fails on every boot with MH_ERROR_ALREADY_CREATED
        # (same target 0x1400EAEF0 as the CSysDLL_GetSymbol hook installed earlier), so making it
        # required would stop every server. Moving a hook between the classes is a policy change
        # and has to change this list.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")

        calls = re.findall(
            r'InstallBootDetour\([^;]*?"([^"]+)"\s*,\s*BootHookRequirement::(k\w+)\s*\)\s*;', body)
        self.assertEqual(len(calls), len(re.findall(r"\bInstallBootDetour\s*\(", body)),
                         "an InstallBootDetour call did not parse; the classification below would miss it")
        optional = {name for name, kind in calls if kind == "kOptional"}
        required = {name for name, kind in calls if kind == "kRequired"}
        self.assertEqual(optional, {"EchoVR::GetProcAddress", "EchoVR::SetWindowTextA_"})
        self.assertEqual(required, {
            "EchoVR::NetGameSwitchState",
            "EchoVR::LoadLocalConfig",
            "EchoVR::CJsonGetFloat",
            "EchoVR::HttpConnect",
            "EchoVR::JsonValueAsString",
        })

    def test_shutdown_thread_never_touches_the_callback_registry(self):
        # Issue #44: the graceful-shutdown thread called self->Unregister(), which reaches
        # UnregisterAllCallbacks -> GetCallbackRegistry() and EchoVR::BroadcasterUnlisten. The
        # registry is documented game-thread-only (server_context.h) and BroadcasterUnlisten takes
        # no lock (echovr.exe 0x140f8df20). The shutdown thread now hands that work to Update()
        # through MainThreadHandoff; its own fallback must skip the registry.
        source = (ROOT / "src/runtime/server/gameserver.cpp").read_text()
        # Comment-stripped (#123): extract_braced_function strips comments for
        # every caller, so a commented-out call site with a decoy real
        # statement nearby can't satisfy these regexes — a raw substring
        # match can't tell "the call is here" from "the call is described in
        # a comment above the decoy".
        shutdown = extract_braced_function(source, "void GameServerLib::BeginGracefulShutdown(")

        for forbidden in (r"\bUnregister\s*\(\s*\)", r"\bUnregisterAllCallbacks\s*\(",
                          r"\bGetCallbackRegistry\s*\(", r"\bUnregisterFromServerDb\s*\(\s*true"):
            self.assertNotRegex(shutdown, forbidden,
                                "the shutdown thread reaches the game-thread-only callback registry")
        self.assertRegex(shutdown, r"m_gameThreadHandoff\.RunOnServicingThread\(",
                         "subject vanished: the shutdown thread no longer hands off to the game thread")
        self.assertRegex(shutdown, r"ShutdownUnregisterOnGameThread\s*\(")

        off_thread = extract_braced_function(source, "void GameServerLib::ShutdownUnregisterOffGameThread(")
        self.assertRegex(off_thread, r"UnregisterFromServerDb\s*\(\s*false\s*\)")
        for forbidden in (r"\bUnregister\s*\(\s*\)", r"\bUnregisterAllCallbacks\s*\(",
                          r"\bGetCallbackRegistry\s*\(", r"\bUnregisterFromServerDb\s*\(\s*true"):
            self.assertNotRegex(off_thread, forbidden)

        # The skip must be real: the registry action is only built when asked for.
        impl = extract_braced_function(source, "void GameServerLib::UnregisterFromServerDb(")
        self.assertRegex(impl, r"if\s*\(\s*touchCallbackRegistry\s*\)\s*unregisterCallbacks\s*=")

        # And the game thread must actually service the hand-off, or every shutdown times out.
        update = extract_braced_function(source, "VOID GameServerLib::Update(")
        self.assertRegex(update, r"m_gameThreadHandoff\.Service\(\)")

    def test_broadcaster_callbacks_record_their_owner_before_listening(self):
        # Issue #117: ba6b5f0 set CallbackRegistry::broadcasterOwner in RegisterBroadcasterCallbacks;
        # merge 033b303 took the other parent's body and dropped it. UnregisterBroadcasterCallbacks
        # only calls EchoVR::BroadcasterUnlisten when the live owner equals the recorded one, so with
        # the owner null every unregister silently skipped the game and only cleared the struct. No
        # C++ test links gameserver.cpp, so the wiring is pinned here.
        source = (ROOT / "src/runtime/server/gameserver.cpp").read_text()
        # Comment-stripped (#123): same reasoning as the #44 sensor above — a
        # commented-out RecordBroadcasterOwner call with a `owner = nullptr;`
        # decoy nearby can't satisfy the substring these regexes look for,
        # because extract_braced_function strips comments before returning.
        register = extract_braced_function(source, "void GameServerLib::RegisterBroadcasterCallbacks(")
        record = re.search(r"\bGameServer::RecordBroadcasterOwner\s*\(\s*\*m_context\s*\)", register)
        self.assertIsNotNone(record, "RegisterBroadcasterCallbacks no longer records the callback owner")
        first_listen = re.search(r"\bListenForBroadcasterMessage\s*\(", register)
        self.assertIsNotNone(first_listen, "subject vanished: no ListenForBroadcasterMessage calls")
        self.assertLess(record.start(), first_listen.start(),
                        "the owner must be recorded before the first handle is registered")

        # Registration, the recorded owner and the unregister guard must all name the same
        # broadcaster (the lobby's), or the guard rejects every handle again.
        listen = extract_braced_function(source, "uint16_t ListenForBroadcasterMessage(")
        self.assertRegex(listen, r"BroadcasterListen\(\s*lobby->broadcaster\s*,")
        unregister = extract_braced_function(source, "void GameServerLib::UnregisterAllCallbacks(")
        self.assertRegex(unregister, r"liveOwner\s*=\s*lobby\s*!=\s*nullptr\s*\?\s*lobby->broadcaster\s*:")
        helper_source = (ROOT / "src/runtime/server/callback_unregistration.cpp").read_text()
        helper = extract_braced_function(helper_source, "EchoVR::Broadcaster* RecordBroadcasterOwner(")
        self.assertRegex(helper, r"lobby\s*!=\s*nullptr\s*\?\s*lobby->broadcaster\s*:")
        self.assertRegex(helper, r"\.broadcasterOwner\s*=")

    def test_bridge_never_closes_a_remote_on_unrequire(self):
        # d0190c4/dd1e9e7 (2026-09-14) closed the remote websocket whenever STcpConnectionUnrequireEvent
        # arrived in server mode, as an experiment (refuted in 79e27d5). Nakama sends that event on
        # the config connection as well, so the config connection was closed before the game's
        # post-login config requests (battle pass, store, eula); they were never delivered and
        # NetGame never left "logging in" (real Windows 2026-10-01 02:17Z: "game->server message NOT
        # delivered: conn=0 (config)"). The bridge must not react to this symbol at all, so the
        # symbol must not appear in it; other remote closes are not covered here.
        source = (ROOT / "src/runtime/compat/ws_bridge.cpp").read_text()

        self.assertNotIn("0x43e6963ac76beee4", source)


if __name__ == "__main__":
    unittest.main()
