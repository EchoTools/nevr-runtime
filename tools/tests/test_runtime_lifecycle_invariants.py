import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def extract_braced_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function body: {signature}")


class RuntimeLifecycleInvariantTest(unittest.TestCase):
    def test_dllmain_does_not_run_plugin_shutdown_or_unload(self):
        source = (ROOT / "src/runtime/lifecycle/dllmain.cpp").read_text()
        body = extract_braced_function(source, "BOOL APIENTRY DllMain(")

        self.assertNotRegex(body, r"\bUnloadPlugins\s*\(")
        self.assertNotIn("NvrPluginShutdown", body)

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
