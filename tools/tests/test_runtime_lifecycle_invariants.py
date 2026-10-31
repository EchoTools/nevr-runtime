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



def gameserver_text() -> str:
    """The GameServerLib implementation, which is split across these files (#47)."""
    names = ("gameserver.cpp", "gameserver_callbacks.cpp", "gameserver_telemetry.cpp", "gameserver_serverdb.cpp")
    return "\n".join((ROOT / "src/runtime/server" / n).read_text() for n in names)


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
    def test_normal_exit_reports_resource_buffers_before_process_termination(self):
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        client_exit = extract_braced_function(source, "VOID WINAPI ExitProcessHook(")
        server_exit = extract_braced_function(source, "void PerformGracefulShutdown(")
        self.assertRegex(client_exit,
                         r"ShutdownResourceOverride\s*\(\s*\)\s*;\s*OriginalExitProcess\s*\(\s*uExitCode\s*\)")
        self.assertRegex(server_exit,
                         r"ShutdownResourceOverride\s*\(\s*\)\s*;\s*ForceFatalExit\s*\(\s*exitCode\s*\)")

    def test_the_crash_attribution_names_both_nevr_images_through_the_policy_not_a_literal(self):
        # N70 after the host/runtime split (#853): the handler runs in nevr-runtime.dll, the game imports
        # BugSplat64.dll, and a fault in either is NEVR's. The module cache holds every loaded DLL (Wine and DXVK
        # too), so the name decides, through one function the GTest drives.
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        vectored = extract_braced_function(source, "LONG WINAPI BreakpointVEH(")
        self.assertRegex(vectored, r"nevr_veh_policy::IsNevrImageName\s*\(\s*g_moduleCache\[m\]\.name\s*\)")
        self.assertNotIn("BugSplat64", strip_comments(source), "the attribution compares names through IsNevrImageName only")
        self.assertNotIn("nevr-runtime", strip_comments(source))
        policy = strip_comments((ROOT / "src/runtime/lifecycle/veh_policy.h").read_text())
        names = extract_braced_function(policy, "inline bool IsNevrImageName(")
        self.assertEqual(sorted(re.findall(r'"([^"]+\.dll)"', names)), ["BugSplat64.dll", "nevr-runtime.dll"])
        self.assertIn("_stricmp", names, "module names are matched without case")

    # Exceptions do not cross the DLL boundary (#853, decision 9). The ellipsis catch is the one the C++ addendum
    # allows only at such a boundary, so each one carries its reason on the line, and there are no others here.
    def test_every_ellipsis_catch_at_the_two_dll_boundaries_says_why(self):
        for name in ("src/runtime/host/runtime_load.cpp", "src/runtime/lifecycle/export_guard.h"):
            with self.subTest(file=name):
                lines = (ROOT / name).read_text().splitlines()
                hits = [i for i, line in enumerate(lines) if re.search(r"catch\s*\(\s*\.\.\.\s*\)", line)]
                self.assertTrue(hits, f"{name} has no ellipsis catch at its boundary")
                for i in hits:
                    text = lines[i] + (lines[i - 1] if i else "")
                    self.assertRegex(text, r"//.*must not cross the DLL boundary",
                                     f"{name}:{i + 1}: an ellipsis catch needs its justification on the line")

    def test_the_runtime_export_entries_all_go_through_the_one_guard(self):
        source = strip_comments((ROOT / "src/runtime/lifecycle/runtime_exports.cpp").read_text())
        self.assertNotRegex(source, r"\btry\b|\bcatch\b", "the entries stop exceptions in export_guard.h only")
        entries = re.findall(r"\bGuarded(?:Void)?\s*\(", source)
        table = re.findall(r"table\.\w+\s*=\s*&", source)
        self.assertEqual(len(table), 16)
        self.assertEqual(len(entries), len(table), "every table entry calls Guarded or GuardedVoid once")

    def test_the_runtimes_initialize_runs_behind_the_guard(self):
        # The host calls initialize from its detour: no exception may unwind into it.
        source = strip_comments((ROOT / "src/runtime/lifecycle/runtime_entry.cpp").read_text())
        self.assertNotRegex(source, r"\btry\b|\bcatch\b", "the guard in export_guard.h is the one place that catches")
        body = extract_braced_function(source, "std::int32_t Initialize(")
        self.assertRegex(body, r"\bGuardedInitialize\s*\(\s*&nevr_runtime_exports::ReportThrew\b")
        for call in ("ValidateHostContext(", "InitializeGameModule(", "ArmGameMain("):
            self.assertIn(call, body)
            self.assertGreater(body.index(call), body.index("GuardedInitialize("), f"{call} runs outside the guard")

    def test_dllmain_does_not_run_plugin_shutdown_or_unload(self):
        source = (ROOT / "src/runtime/lifecycle/dllmain.cpp").read_text()
        body = extract_braced_function(source, "BOOL APIENTRY DllMain(")

        self.assertNotRegex(body, r"\bUnloadPlugins\s*\(")
        self.assertNotIn("NvrPluginShutdown", body)

    def test_dllmain_frees_the_real_dbgcore_only_on_dynamic_unload(self):
        # Issue #40: FreeLibrary(g_realDbgCore) ran on process termination too (lpReserved != NULL).
        # Calling FreeLibrary from DllMain during termination can leave a module in use after the
        # system ran its termination code; only a dynamic unload (lpReserved == NULL) frees it.
        # The dbgcore proxy is the host's (#853): BugSplat64.dll's DllMain owns it.
        source = (ROOT / "src/runtime/host/host_dllmain.cpp").read_text()
        body = extract_braced_function(source, "BOOL APIENTRY DllMain(")

        detach = body.index("case DLL_PROCESS_DETACH")
        section = body[detach:]
        frees = [m.start() for m in re.finditer(r"\bFreeLibrary\s*\(\s*g_realDbgCore\s*\)", body)]
        self.assertEqual(len(frees), 1, "subject vanished: expected exactly one FreeLibrary(g_realDbgCore)")
        free = frees[0] - detach
        self.assertGreater(free, 0, "FreeLibrary(g_realDbgCore) is not in the DLL_PROCESS_DETACH case")
        guard = re.search(r"if\s*\(\s*lpReserved\s*==\s*NULL\s*&&\s*g_realDbgCore\s*\)\s*\{", section)
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

    def test_the_dbgcore_refusal_logs_a_line_before_each_fatal_error(self):
        # Issue #726: on a client FatalError only shows a dialog, so the refusal must leave a record first.
        source = strip_comments((ROOT / "src/runtime/lifecycle/boot.cpp").read_text())
        fatals = [m.start() for m in re.finditer(r'FatalError\(', source)
                  if '"dbgcore.dll hijack detected"' in source[m.start():m.start() + 600]]
        self.assertEqual(len(fatals), 2, "subject vanished: expected the server and client dbgcore refusals")
        for fatal in fatals:
            last_log = source.rfind("Log(", 0, fatal)
            self.assertTrue(last_log > source.rfind("FatalError(", 0, fatal),
                            "the dbgcore FatalError has no Log( since the previous FatalError")
            window = source[last_log:fatal]
            self.assertIn("LogLevel::Error", window)
            self.assertIn('"[NEVR.PATCH] dbgcore.dll present', window)

    def test_server_shutdown_uses_console_handler_without_crt_signals(self):
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        stripped = strip_comments(source)
        self.assertNotIn("PosixSignalHandler", stripped)
        self.assertNotRegex(stripped, r"\bsignal\s*\(")
        self.assertIn("SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)", stripped)
        handler = extract_braced_function(stripped, "static BOOL WINAPI ConsoleCtrlHandler(")
        self.assertIn("PerformGracefulShutdown(0)", handler)

    def test_early_boot_never_reads_config(self):
        # service_config.cpp NevrCfg() loads config.yaml on first access and documents that first
        # access is after the CLI is parsed (g_isServer, -config-file). 3d4a994 called
        # NevrCfgSocialFacadeEnabled() from the boot sequence and a real server with a config.yaml
        # died silently right after "minhook initialized".
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")

        self.assertNotRegex(body, r"\bNevrCfg\w*\s*\(")
        self.assertNotRegex(body, r"\bNevrGame\w*\s*\(")

    def test_a_value_option_never_takes_an_option_as_its_value(self):
        # #587: `-timestep -no-console` lost -no-console because boot.cpp took argv[i + 1] unchecked.
        # Every value option reads its value through nevr_cli::OptionValue (core/cli_option_value.h).
        boot = strip_comments((ROOT / "src/runtime/lifecycle/boot.cpp").read_text())

        self.assertNotRegex(boot, r"i\s*\+\s*1\s*<\s*argc", "a raw lookahead past the option is back")
        self.assertNotRegex(boot, r"argv\[\s*(\+\+i|i\s*\+\s*1)\s*\]", "argv[i + 1] is read unchecked")
        # -config (twice: the early pass and the main parse), -telemetry-rate, -region, -trace-exports, -timestep.
        self.assertGreaterEqual(len(re.findall(r"nevr_cli::OptionValue\(", boot)), 6)

    def test_the_boot_log_names_the_host_environment_in_the_boot_phase(self):
        # #673: a crash report names the Wine version from the log. The line is written through the boot
        # tee (loader lock held), after the tee opens and before it first closes.
        source = strip_comments((ROOT / "src/runtime/lifecycle/initialize.cpp").read_text())
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")
        line = body.index("nevr_host_environment::BootLine(nevr_system_info::Get())")
        self.assertLess(body.index("nevr_boot_log_tee::Init()"), line,
                        "the host environment line must come after Init(), but it is written before the tee opens")
        self.assertLess(line, body.index("nevr_boot_log_tee::Close()"),
                        "the host environment line must come before Close(), but it is written after the tee closes")
        self.assertRegex(body[max(0, line - 40):line], r"TeeFprintf\(", "the line must go through the boot tee")

    def test_the_dll_load_hook_reports_through_the_log_never_raw_stderr(self):
        # #654: four [NEVR.DLLHOOK] lines went to fprintf(stderr) and never reached the JSONL log. The
        # hook's own lines go through ReportLine (Log() in the boot phase and outside the loader lock, held
        # until released under it) or, for the install report, the boot tee.
        source = strip_comments((ROOT / "src/runtime/hook/dll_load_hook.cpp").read_text())

        self.assertNotRegex(source, r"fprintf\s*\(\s*stderr", "a hook line is written to raw stderr again")
        self.assertGreaterEqual(len(re.findall(r"\bReportLine\s*\(\s*EchoVR::LogLevel::", source)), 4)

    def test_graceful_shutdown_writes_the_dll_hook_lines_held_under_the_loader_lock(self):
        # #688: a line made by a load under the loader lock waits in dll_load_hook.cpp's buffer. The graceful
        # shutdown writes it, outside the lock, before the process is ended.
        source = strip_comments((ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text())
        body = extract_braced_function(source, "void PerformGracefulShutdown(")
        flush = body.index("nevr_dll_load_hook::FlushPendingLines()")
        self.assertLess(flush, body.index("ForceFatalExit(exitCode)"),
                        "the held lines must be written before ForceFatalExit ends the process")

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
        # The two Hooking::Attach results (r1, r2) must be captured in a variable, not dropped —
        # a new bare `Hooking::Attach(...)` statement would silently discard its result the same
        # way the original seven PatchDetour calls did.
        attach_calls = re.findall(r"\bHooking::Attach\s*\(", body)
        captured_calls = re.findall(r"\bBOOL\s+r\d+\s*=\s*Hooking::Attach\s*\(", body)
        self.assertEqual(len(attach_calls), len(captured_calls),
                         "a Hooking::Attach call's result is not captured in a variable")

    def test_veh_boot_line_comes_from_the_result_of_install_veh(self):
        # The boot line must distinguish an early Wine deferral from an installation failure.
        source = strip_comments((ROOT / "src/runtime/lifecycle/initialize.cpp").read_text())
        self.assertRegex(source, r"\bconst\s+nevr_veh_policy::InstallResult\s+vehResult\s*=\s*InstallVEH\s*\(\s*\)\s*;")
        self.assertRegex(source, r"nevr_veh_policy::BootLine\(\s*vehResult\s*\)")
        self.assertNotIn("veh installed", source,
                         "initialize.cpp hard-codes the veh boot line; take it from nevr_veh_policy::BootLine")

    def test_dll_load_hooks_installed_line_is_gated_on_the_install_outcome(self):
        # #632 (N127's shape): Install() returns how many LoadLibrary variants it hooked. The boot line
        # says "installed" only for all of them; a partial install prints a line that names the count, and
        # (#632: the hooks are required) fails the boot with the count as its cause.
        # InitializeAfterGameImageGuard cannot run in a GTest, so this pins the source; the verdict itself
        # is tested in test_dll_load_hook.cpp (DllLoadHookBootVerdict).
        source = strip_comments((ROOT / "src/runtime/lifecycle/initialize.cpp").read_text())
        begin = source.index("installing DLL load hooks")
        flat = re.sub(r"\s+", " ", source[begin : source.index("InstallHeadlessGraphicsHooks()", begin)])

        def need(pattern, message):
            # assertRegex would dump the whole block on a failure; a short message names the broken rule.
            found = re.search(pattern, flat)
            self.assertIsNotNone(found, message)
            return found

        count = need(r"const int (\w+) = nevr_dll_load_hook::Install\(\);", "Install() result not kept").group(1)
        v = need(r"const nevr_boot_hook_policy::LoadLibraryVerdict (\w+) = "
                 r"nevr_boot_hook_policy::JudgeLoadLibraryHooks\(\s*" + count +
                 r", nevr_dll_load_hook::kLoadLibraryVariantCount\);",
                 "verdict must be JudgeLoadLibraryHooks(count, kLoadLibraryVariantCount)").group(1)
        need(r"if \(" + v + r"\.complete\) \{ nevr_boot_log_tee::TeeFprintf\(\"\[NEVR\.PATCH\] info; "
             r"dll load hooks installed\\n\"\); \} else \{",
             "'dll load hooks installed' must be the complete branch only")
        self.assertEqual(source.count("dll load hooks installed"), 1)
        need(r"\[NEVR\.PATCH\] warn; dll load hooks PARTIAL", "the PARTIAL line must carry warn; after its tag (logging.md Rule 12)")
        need(r"dll load hooks PARTIAL \(%d/%d\).*?\\n\", " + count + r", nevr_dll_load_hook::kLoadLibraryVariantCount\);",
             "PARTIAL arguments must be the installed count, then kLoadLibraryVariantCount")
        need(r"if \(" + v + r"\.failsBoot\) \{ g_bootHookFailed = true; NoteBootHookFailureCause\(" + v +
             r"\.cause\); \}", "a failing verdict must set g_bootHookFailed and its cause")

    def test_mic_provider_drops_stale_audio_at_the_first_game_read_and_caps_latency(self):
        # #95: the ring fills before the game's first read of a stream. The first MicRead drops that
        # backlog (MicAvailable polls do not), the overflow warning is for a game that read and then
        # stopped, and the ring is capped at 200 ms.
        source = strip_comments((ROOT / "src/runtime/patch/mic_provider.cpp").read_text())
        read = extract_braced_function(source, "uint64_t nevr_mic_provider::MicRead(")
        self.assertRegex(read, r"\bNoteGameReader\s*\(\s*\)", "MicRead must drop the backlog")
        # MicAvailable is a poll the game makes from the moment capture starts; it must not drop or latch.
        avail = extract_braced_function(source, "uint64_t nevr_mic_provider::MicAvailable(")
        self.assertNotRegex(avail, r"\bNoteGameReader\s*\(", "MicAvailable must not drop the backlog")
        # #95: the capture side is in the periodic line and the overflow warning is counted, not one-shot.
        # The gate is polled on every packet while the game reads, so its window slides while nothing waits.
        self.assertRegex(source, r"!g_ring\.ReaderActive\(\)[\s\S]{0,200}g_overflowGate\.Poll\(")
        self.assertNotRegex(source, r"result\.ringOverflow", "the gate is polled only on overflow packets")
        self.assertNotIn("g_ringOverflowLogged", source)
        # The overflow line reports what the game did in the window (stalled or slow), not that it "is not
        # draining"; the window's read counters reach the gate from the live counters.
        self.assertIn("ring overflow reader=%s", source)
        self.assertNotIn("is not draining", source)
        self.assertRegex(source, r"flow\.readCalls\s*=\s*g_readCalls\.load\(\)")
        self.assertRegex(source, r"flow\.samplesRead\s*=\s*g_samplesRead\.load\(\)")
        self.assertRegex(source, r"g_overflowGate\.Poll\(\s*flow\s*,")
        for field in ("packets=", "silent=", "discontinuities=", "samples_pushed=", "samples_dropped=",
                      "max_poll_gap_ms="):
            self.assertIn(field, source, field)
        self.assertIn("AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY", source)
        cap = re.search(r"kRingCapacitySamples\s*=\s*(\d+)\s*;", source)
        self.assertIsNotNone(cap)
        self.assertLessEqual(int(cap.group(1)), 9600)

    def test_the_recently_met_line_is_logged_after_the_list_is_stored(self):
        # #676: the scenario harness waits for `recently met list count=` and then reads the list, so the
        # line must not come before RecentlyMet().SetList (a wait that fires early reads the old list).
        source = strip_comments((ROOT / "src/runtime/compat/ws_bridge.cpp").read_text())
        store = source.index("nevr_social_roster::RecentlyMet().SetList(")
        line = source.index("[NEVR.SOCIAL] recently met list count=")
        self.assertLess(store, line, "the recently-met log line is written before the list is stored")

    def test_radpluginshutdown_guard_lives_in_the_one_detour_on_the_symbol_resolver(self):
        # #93/#94: 0x1400EAEF0 takes one detour (CSysDLL_GetSymbol). The server-only
        # RadPluginShutdown guard has to be inside that hook; a second detour on the
        # same target never installs.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static void* CSysDLL_GetSymbolHook(")
        self.assertRegex(body, r"g_isServer\s*&&[^)]*\"RadPluginShutdown\"")
        self.assertIn('"Users"', body)
        self.assertNotIn("GetProcAddressHook", strip_comments(source))
        self.assertNotRegex(strip_comments(source), r"InstallBootDetour\(\s*&EchoVR::GetProcAddress")

    def test_only_reviewed_boot_hooks_are_optional(self):
        # A required hook that fails makes a server refuse to start (boot.cpp: g_bootHookFailed ->
        # ServerFatal). Moving a hook between the classes is a policy change and has to change
        # this list.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")

        calls = re.findall(
            r'InstallBootDetour\([^;]*?"([^"]+)"\s*,\s*BootHookRequirement::(k\w+)\s*\)\s*;', body)
        self.assertEqual(len(calls), len(re.findall(r"\bInstallBootDetour\s*\(", body)),
                         "an InstallBootDetour call did not parse; the classification below would miss it")
        optional = {name for name, kind in calls if kind == "kOptional"}
        required = {name for name, kind in calls if kind == "kRequired"}
        self.assertEqual(optional, {"EchoVR::SetWindowTextA_"})
        self.assertEqual(required, {
            "EchoVR::NetGameSwitchState",
            "EchoVR::LoadLocalConfig",
            "EchoVR::CJsonGetFloat",
            "EchoVR::HttpConnect",
            "EchoVR::JsonValueAsString",
        })

    def test_telemetry_socket_refreshes_its_token_on_a_reconnect_401(self):
        # Issue #114: TelemetryStreamer set the Authorization header once, and ixwebsocket's automatic
        # reconnect re-presented it after it expired. The Error handler must feed BearerReconnectAuth,
        # and the ServerDB-token fallback must install a refresher (a configured telemetry_token must not).
        streamer = strip_comments((ROOT / "src/runtime/server/telemetry_streamer.cpp").read_text())
        self.assertRegex(streamer, r"m_bearerAuth\.Attach\s*\(")
        self.assertRegex(streamer, re.compile(
            r"case ix::WebSocketMessageType::Error:(?:(?!WebSocketMessageType::Message).)*?"
            r"m_bearerAuth\.OnError\s*\(\s*msg->errorInfo\.http_status", re.S))
        server = gameserver_text()
        request = extract_braced_function(server, "VOID GameServerLib::RequestRegistration(")
        self.assertRegex(request, r"\bConnectTelemetry\s*\(\s*wsToken\s*\)")
        telemetry = (ROOT / "src/runtime/server/gameserver_telemetry.cpp").read_text()
        fallback = extract_braced_function(telemetry, "void GameServerLib::ConnectTelemetry(")
        self.assertRegex(fallback, re.compile(
            r"token = wsToken;(?:(?!m_telemetry->Connect).)*?m_telemetry->SetBearerTokenRefresher\s*\(", re.S))

    def test_console_handler_is_rearmed_after_the_game_installs_its_own(self):
        # Issue #102: the game installs its console ctrl handler (echovr.exe 0x1400dcbb0, from
        # CR15Game::InitRenderWindowFromEngineFlags) after our boot-time re-arm, so its handler sat in
        # front and swallowed CTRL+C. The installer is hooked and our handler re-armed once it returns.
        source = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        hook = extract_braced_function(source, "static INT64 GameConsoleHandlerInstallHook(")
        self.assertRegex(hook, r"OriginalGameConsoleHandlerInstall\s*\(")
        self.assertRegex(hook, r"\bRearmConsoleCtrlHandler\s*\(\s*\)")
        install = extract_braced_function(source, "void InstallConsoleCtrlHandler(")
        self.assertRegex(install, r"\bInstallGameConsoleHandlerRearmHook\s*\(\s*\)")

    def test_the_shared_plugin_logger_is_leveled(self):
        # Issue #34: each plugin hand-rolled a stderr wrapper or used a levelless PluginLog. The SDK's
        # NEVR_DEFINE_PLUGIN_LOG writes one leveled line per call.
        header = (ROOT / "sdk/include/nevr/plugin_logger.h").read_text()
        for level in ("INFO", "WARNING", "ERROR"):
            self.assertIn(f'"{level}"', header)

    def test_plugins_log_through_the_host_log(self):
        # Issue #598: a plugin had no way into the host's structured Log(). The context carries the
        # plugin's id and the host's log function, the loader fills both into every context it passes a
        # plugin (init, per-frame, state change), and the shared plugin logger binds to them at init.
        iface = strip_comments((ROOT / "sdk/include/nevr/plugin_interface.h").read_text())
        self.assertRegex(iface, r"const char\*\s+plugin_id;")
        self.assertRegex(iface, r"NevrLogFn\s+log;")
        loader = strip_comments((ROOT / "src/runtime/ext/plugin_loader.cpp").read_text())
        self.assertRegex(loader, r"ctx\.log\s*=\s*HostPluginLog\s*;")
        self.assertRegex(loader, r"ctx\.plugin_id\s*=")
        self.assertEqual(len(re.findall(r"\bContextFor\(\s*\*?\w+\s*,", loader)), 3,
                         "init, TickPlugins and NotifyPluginsStateChange each pass the plugin its own context")
        logger = strip_comments((ROOT / "sdk/include/nevr/plugin_logger.h").read_text())
        self.assertRegex(logger, r"binding\.log\s*\(\s*binding\.id\s*,")

    def test_broadcaster_hook_entries_are_counted_only_by_hook_liveness(self):
        # Issue #33: mode_patches.cpp kept its own entry counters and a periodic log line that
        # duplicated nevr_hook_liveness::Report for the same two hooks. HookLiveness is the one instrument.
        patches = strip_comments((ROOT / "src/runtime/patch/mode_patches.cpp").read_text())
        for gone in ("g_listenHookEntries", "g_dispatchHookEntries", "LogBroadcasterHookStats"):
            self.assertNotIn(gone, patches)
        for hook, marker in (("static INT16 CBroadcasterListenHook(", "kBroadcasterListen"),
                             ("static VOID CBroadcasterReceiveLocalEventHook(", "kBroadcasterReceiveLocal")):
            body = extract_braced_function(patches, hook)
            self.assertIn(f"nevr_hook_liveness::Mark(nevr_hook_liveness::{marker})", body)
            self.assertRegex(body, r"chainReadable\s*=\s*nevr_broadcaster_guard::TryReadListenerTable\(")
            self.assertRegex(body, r"if\s*\(\s*!chainReadable\b")
        tick = strip_comments((ROOT / "src/runtime/frame/tick.cpp").read_text())
        self.assertNotIn("LogBroadcasterHookStats", tick)
        for gone in ("broadcaster_hook_stats.cpp", "broadcaster_hook_stats.h"):
            self.assertFalse((ROOT / "src/runtime/patch" / gone).exists(), f"{gone} has no caller and was deleted")
        self.assertIn('nevr_hook_liveness::Report("periodic")', tick)

    def test_broadcaster_hooks_report_rejected_chains_before_original_call(self):
        patches = strip_comments((ROOT / "src/runtime/patch/mode_patches.cpp").read_text())
        for hook, original in (("static INT16 CBroadcasterListenHook(", "OriginalCBroadcasterListen("),
                               ("static VOID CBroadcasterReceiveLocalEventHook(", "OriginalCBroadcasterReceiveLocalEvent(")):
            body = extract_braced_function(patches, hook)
            guard = body.find("nevr_broadcaster_guard::TryReadListenerTable(")
            self.assertGreaterEqual(guard, 0, hook)
            self.assertLess(guard, body.find(original), hook)
            self.assertNotRegex(body, r"\*\s*arg1\b")
            self.assertRegex(body, re.compile(r"if\s*\(\s*!chainReadable\s*\|\|.*?kMaximumUserAddress", re.S))
            self.assertIn('"pointer_chain"', body)
            warning = body.find("Log(EchoVR::LogLevel::Warning,", guard)
            self.assertGreater(warning, guard, hook)
            self.assertLess(warning, body.find(original), hook)
            self.assertNotRegex(body[guard:warning], r"\breturn\b", hook)

    def test_getsymbol_hook_validates_its_prologue(self):
        # Issue #254: the CSysDLL_GetSymbol detour (echovr.exe 0x1400eaef0) was written blind. Binary
        # patches require prologue validation (AGENTS.md Guardrails); a mismatch marks the boot hook failed.
        source = (ROOT / "src/runtime/lifecycle/initialize.cpp").read_text()
        body = extract_braced_function(source, "static VOID InitializeAfterGameImageGuard(")
        match = re.search(r"sym_target[^;]*;(?P<rest>.*?)hooked name=CSysDLL_GetSymbol", body, re.S)
        self.assertIsNotNone(match)
        rest = match.group("rest")
        self.assertRegex(rest, r"memcmp\(\s*sym_target\s*,\s*kGetSymbolPrologue")
        self.assertLess(rest.index("memcmp("), rest.index("MH_CreateHook("))
        self.assertIn("prologue_mismatch", rest)
        self.assertIn("g_bootHookFailed = true", rest.split("MH_CreateHook(")[0])

    def test_bridge_connection_lines_carry_the_connection_label(self):
        # Issue #48: only close/disconnect lines named the connection (config/login/matchmaker); the
        # open, login-injected and game-connected lines gave the bare number.
        source = strip_comments((ROOT / "src/runtime/compat/ws_bridge.cpp").read_text())
        for anchor in ("Proxy: game connected (conn=", "login injected xpid=", "Remote open (conn=",
                       "could not percent-encode URL credentials"):
            starts = [m.start() for m in re.finditer(re.escape(anchor), source)]
            self.assertTrue(starts, anchor)
            for start in starts:
                statement = source[start:source.index(");", start)]
                self.assertIn("ConnLabel(", statement, f"{anchor!r} logs a bare connection number")

    def test_platform_compat_reports_a_failed_xmlhttp_creation_loudly(self):
        # Issue #242: the pass-through line logged a failed CoCreateInstance (hr=0x80040154) at Info.
        # A failure is Warning or higher; an observe-only hook that fails to attach is a Warning too.
        source = (ROOT / "src/modules/platform-compat/src/platform_compat.cpp").read_text()
        hook = extract_braced_function(source, "HRESULT WINAPI CoCreateInstanceHook(")
        self.assertRegex(hook, r"Log\(SUCCEEDED\(hr\)\s*\?\s*EchoVR::LogLevel::Info\s*:\s*EchoVR::LogLevel::Warning")
        install = extract_braced_function(source, "static bool InstallMsxml6PassThroughHook(")
        attach_failure = install.split("Hooking::Attach", 1)[1].split("return false;", 1)[0]
        self.assertIn("EchoVR::LogLevel::Warning", attach_failure)
        self.assertNotIn("EchoVR::LogLevel::Error", attach_failure)

    def test_verify_server_supplies_a_config_when_the_install_has_none(self):
        # Issue #245: a server with no config.yaml gets no embedded defaults and refuses to boot without a
        # login bridge (#16), so every verify-server.sh flag set died at boot. The script deploys the
        # offline-boot config the Windows-VM rig uses, only when the install has none. The runtime reads config.yaml
        # from the nevr root (#1255), so that is where it goes; an install with an old _local/config.yaml is left alone.
        script = (ROOT / "verify-server.sh").read_text()
        self.assertRegex(script, r'if \[ ! -e "\$GAME_ROOT/echovr/_local/config\.yaml" \] && \[ ! -e "\$GAME_DIR/\$NEVR_ROOT_REL/config\.yaml" \]')
        self.assertIn("allow_offline_server: true", script)
        self.assertIn('socket_uri: ""', script)  # #843: an explicit empty socket_uri takes no embedded default
        self.assertRegex(script, r'deploy "\$OUT/offline-config\.yaml" "\$GAME_DIR/\$NEVR_ROOT_REL/config\.yaml"')

    def test_token_mints_are_serialized(self):
        # Issue #246: the ServerDB refresher, the telemetry refresher and RequestRegistration all
        # reach RefreshAuthToken -> SaveAuthToken (an unlocked truncating write of .credentials.json).
        server = gameserver_text()
        acquire = extract_braced_function(server, "std::string AcquireServerDbToken(")
        self.assertRegex(acquire, r"nevr_serverdb_auth::RunSerializedMint\s*\(")
        helper = (ROOT / "src/runtime/server/serialized_mint.h").read_text()
        self.assertRegex(helper, r"std::lock_guard<std::mutex>")

    def test_console_defer_needs_gameserverlib_started(self):
        # Issue #241: re-arming the handler must not set the defer flag unconditionally, or a server that
        # never reached GameServerLib::Initialize defers to a teardown that never ends (watchdog, exit 1).
        # Only GameServerLib::Initialize marks the library started, and the boot sequence does not
        # re-arm before the game has installed its handler.
        recovery = (ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text()
        rearm = extract_braced_function(recovery, "void RearmConsoleCtrlHandler(")
        self.assertNotRegex(rearm, r"s_gameServerLibStarted")
        handler = extract_braced_function(recovery, "static BOOL WINAPI ConsoleCtrlHandler(")
        self.assertRegex(handler, r"nevr_console_ctrl_policy::ShouldDeferToGame\s*\(")
        server = gameserver_text()
        initialize = extract_braced_function(server, "VOID* GameServerLib::Initialize(")
        self.assertRegex(initialize, r"\bNotifyGameServerLibStarted\s*\(\s*\)")
        boot = (ROOT / "src/runtime/lifecycle/boot.cpp").read_text()
        self.assertNotRegex(boot, r"\bRearmConsoleCtrlHandler\s*\(")

    def test_telemetry_disconnect_cancels_the_bearer_refresh_before_stopping(self):
        # Issue #254: stop() joins ixwebsocket's thread, so a 401 mint (up to the 10 s HTTP timeout)
        # started during the stop would stall Disconnect.
        source = strip_comments((ROOT / "src/runtime/server/telemetry_streamer.cpp").read_text())
        body = extract_braced_function(source, "void TelemetryStreamer::Disconnect(")
        self.assertLess(body.index("m_bearerAuth.Cancel()"), body.index("m_ws->stop()"))

    def test_game_main_hook_has_no_c_style_casts(self):
        # Issue #254 (CPP-MINGW addendum: no C-style casts).
        source = strip_comments((ROOT / "src/runtime/lifecycle/crash_recovery.cpp").read_text())
        body = extract_braced_function(source, "void ArmGameMain(")
        self.assertNotRegex(body, r"=\s*\(\s*\w+\s*\*\s*\)\s*\(")

    def test_matchmaking_host_patch_decision_is_stateless(self):
        # Issue #18 / #286 F5: pnsradmatchmaking.dll is unloaded and reloaded mid-session, so every load is
        # patched. That the apply step holds no guard is pinned in test_pnsrad_load_boundary.py (#504); the
        # header's per-image decision holds no state either, so a reloaded image reaches Apply().
        header = strip_comments((ROOT / "src/runtime/patch/matchmaker_host_patch.h").read_text())
        decision = extract_braced_function(header, "std::optional<Result> OnModuleNotification(")
        self.assertNotRegex(decision, r"\bstatic\b|\bonce\b|\bdone\b|\bPatched\b",
                            "OnModuleNotification must stay stateless: a reloaded image is a new image")
        self.assertIn("Apply(", decision)

    def test_runtime_schedules_return_to_lobby_through_the_ttl_hold(self):
        # Issue #58: the ServerDB CODE_ENDED path calls nevr_return_to_lobby::Request (not the game function
        # directly) and the game thread polls the hold once per Update.
        server = strip_comments(gameserver_text())
        call = extract_braced_function(server, "void CallScheduleReturnToLobby(")
        self.assertIn("nevr_return_to_lobby::Request(", call)
        self.assertNotIn("EchoVR::NetGameScheduleReturnToLobby", call)
        update = extract_braced_function(server, "VOID GameServerLib::Update(")
        self.assertRegex(update.lstrip("{ \n"), r"^nevr_return_to_lobby::Poll\(\)")
        boot = strip_comments((ROOT / "src/runtime/lifecycle/boot.cpp").read_text())
        self.assertRegex(boot, r"nevr_return_to_lobby::Configure\(")
        glue = strip_comments((ROOT / "src/runtime/lifecycle/return_to_lobby.cpp").read_text())
        self.assertIn("0x1A89F0", glue)
        self.assertRegex(glue, r"memcmp\(target, kPrologue")
        self.assertLess(glue.index("memcmp(target, kPrologue"), glue.index("PatchDetour("))

    def test_ttl_hold_is_dropped_when_the_detour_is_not_installed_and_logs_once_per_hold(self):
        # Issue #310: a failed detour must leave no TTL behind (the runtime's own CODE_ENDED return
        # would still be held), and the game's per-tick re-request must not log per tick.
        glue = strip_comments((ROOT / "src/runtime/lifecycle/return_to_lobby.cpp").read_text())
        configure = extract_braced_function(glue, "bool Configure(")
        self.assertRegex(configure, r"if \(!armed\)[^}]*SetTtlMs\(0\)")
        poll = extract_braced_function(glue, "void Poll(")
        self.assertIn("PollIfActive(g_policy", poll, "Poll must stay a no-op at TTL 0")
        self.assertNotIn("LiveEntrants()", poll)
        request = extract_braced_function(glue, "void Request(")
        self.assertRegex(request, r"RequestVerdict::Hold\)\s*\{\s*Log\(")

    def test_both_registration_sites_use_the_shared_envelope_builder(self):
        # Issue #46: the initial registration and the post-reconnect re-registration built the same
        # envelope field by field in two places. Both go through BuildRegistrationEnvelope.
        # gameserver_serverdb.cpp holds the initial registration, gameserver_callbacks.cpp the re-registration.
        server = strip_comments(gameserver_text())
        self.assertEqual(len(re.findall(r"nevr_game_server::BuildRegistrationEnvelope\s*\(", server)), 2)
        self.assertNotIn("mutable_game_server_registration()", server)

    def test_a_collapsed_burst_that_goes_quiet_is_reported_at_the_health_tick_and_at_shutdown(self):
        # N78 / #894: the limiter only reports a collapsed count when a later line lands on its slot, so
        # the health tick (windows that expired) and Shutdown (windows still open, before the file closes)
        # close out the rest. The behaviour of TakePending is test_rate_limiter.cpp's; this pins the wiring.
        filt = strip_comments((ROOT / "src/runtime/log/builtin_filter.cpp").read_text())
        helper = extract_braced_function(filt, "static void FlushRateSummaries(")
        self.assertRegex(helper, r"while\s*\(g_rate\.TakePending\(GetEpochSeconds\(\),\s*all,\s*summary,")
        self.assertIn("EmitLine(LOG_LEVEL_INFO, summary,", helper)
        self.assertNotRegex(helper, r"\bLog\s*\(", "a summary goes through the filter's own writer, not Log()")
        health = extract_braced_function(filt, "static void MaybeEmitHealth(")
        self.assertIn("FlushRateSummaries(false);", health)
        self.assertLess(health.index("compare_exchange_strong"), health.index("FlushRateSummaries(false);"),
                        "one thread flushes per health window")
        shutdown = extract_braced_function(filt, "void nevr_builtin_log_filter::Shutdown(")
        self.assertIn("FlushRateSummaries(true);", shutdown)
        self.assertLess(shutdown.index("FlushRateSummaries(true);"), shutdown.index("ShutdownFileLogging();"),
                        "the open windows are written before the log file closes")
        hook = extract_braced_function(filt, "static void __fastcall hook_PrintfImpl(")
        self.assertIn("g_rate.Check(buf, emit_len, GetEpochSeconds(), summary,", hook)

    def test_boot_lines_are_replayed_into_the_main_log_at_first_open_only(self):
        # Issue #5: boot and runtime events are one stream. The main log replays this run's
        # nevr-boot.jsonl lines when it first opens; rotation reopens without replaying; the boot file
        # is read, never deleted (it is the crash spool).
        filt = strip_comments((ROOT / "src/runtime/log/builtin_filter.cpp").read_text())
        init = extract_braced_function(filt, "static void InitFileLogging(")
        self.assertLess(init.index("OpenLogFile()"), init.index("ReplayBootLines()"))
        rotate = extract_braced_function(filt, "static void RotateIfNeeded(")
        self.assertNotIn("ReplayBootLines", rotate)
        replay = extract_braced_function(filt, "static void ReplayBootLines(")
        self.assertIn("nevr_boot_replay::ReadNew(path, GetRunId(), g_boot_cursor", replay)
        self.assertRegex(replay, r"ReadNew\([^;]*\)\)\s*\{\s*BlfLog\(", "an unreadable boot file is reported, not skipped")
        record = extract_braced_function(filt, "static void WriteFileRecord(")
        self.assertEqual(len(re.findall(r"nevr_json_escape::AppendTo\(line, (ts|lvl)", record)), 2,
                         "ts and level come from the parsed boot file and are escaped like the message")
        # The tee stays open until initialize() closes it; the lines it writes after the main log
        # opened are replayed once more, under the file lock, just before the tee closes.
        tail = extract_braced_function(filt, "void nevr_builtin_log_filter::ReplayBootTail(")
        self.assertIn("g_file_mutex", tail)
        self.assertIn("ReplayBootLines()", tail)
        init_cpp = strip_comments((ROOT / "src/runtime/lifecycle/initialize.cpp").read_text())
        self.assertRegex(init_cpp, r"nevr_boot_log_tee::Close\(\);\s*nevr_builtin_log_filter::ReplayBootTail\(\);",
                         "the tail is read after Close(), when nothing can append any more")
        for forbidden in ("remove(", "DeleteFile", "unlink(", "trash"):
            self.assertNotIn(forbidden, replay, "the boot file is the crash spool and is never deleted")
        tee = strip_comments((ROOT / "src/runtime/log/boot_log_tee.cpp").read_text())
        self.assertIn("nevr_boot_lines::Build(", tee)
        # #664: the replay carries each record's own level into the main log, not a literal.
        self.assertRegex(replay, r"WriteFileRecord\(\s*line\.ts\.c_str\(\)\s*,\s*line\.level\.c_str\(\)")
        self.assertIn("GetSystemTime(", tee)
        close = extract_braced_function(tee, "void nevr_boot_log_tee::Close(")
        self.assertNotIn("g_boot_path", close, "Path() must stay valid after Close()")

    def test_shutdown_thread_never_touches_the_callback_registry(self):
        # Issue #44: the graceful-shutdown thread called self->Unregister(), which reaches
        # UnregisterAllCallbacks -> GetCallbackRegistry() and EchoVR::BroadcasterUnlisten. The
        # registry is documented game-thread-only (server_context.h) and BroadcasterUnlisten takes
        # no lock (echovr.exe 0x140f8df20). The shutdown thread now hands that work to Update()
        # through MainThreadHandoff; its own fallback must skip the registry.
        source = gameserver_text()
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
        source = gameserver_text()
        # Comment-stripped (#123): same reasoning as the #44 sensor above — a
        # commented-out RecordBroadcasterOwner call with a `owner = nullptr;`
        # decoy nearby can't satisfy the substring these regexes look for,
        # because extract_braced_function strips comments before returning.
        callbacks = (ROOT / "src/runtime/server/gameserver_callbacks.cpp").read_text()
        register = extract_braced_function(callbacks, "void GameServerLib::RegisterBroadcasterCallbacks(")
        record = re.search(r"\bnevr_game_server::RecordBroadcasterOwner\s*\(\s*\*m_context\s*\)", register)
        self.assertIsNotNone(record, "RegisterBroadcasterCallbacks no longer records the callback owner")
        first_listen = re.search(r"\bListenForBroadcasterMessage\s*\(", register)
        self.assertIsNotNone(first_listen, "subject vanished: no ListenForBroadcasterMessage calls")
        self.assertLess(record.start(), first_listen.start(),
                        "the owner must be recorded before the first handle is registered")

        # Registration, the recorded owner and the unregister guard must all name the same
        # broadcaster (the lobby's), or the guard rejects every handle again.
        listen = extract_braced_function(source, "uint16_t ListenForBroadcasterMessage(")
        self.assertRegex(listen, r"BroadcasterListen\(\s*lobby->broadcaster\s*,")
        unregister = extract_braced_function(callbacks, "void GameServerLib::UnregisterAllCallbacks(")
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
