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
