"""tools/verify_self_check_wiring.py: self-checks are unconditional and no debug query is added by build type (#451)."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "verify_self_check_wiring.py"
COPY = ["src/runtime/compat/ws_bridge.cpp", "src/quest/integration/production_steps.cpp",
        "src/runtime/lifecycle/config.cpp", "src/runtime/lifecycle/crash_recovery.cpp", "src/quest/redirect/service_redirector.cpp",
        "src/quest/integration/self_check_wiring.cpp", "src/quest/sentinel/quest_config.h",
        "src/runtime/compat/self_check.h", "src/core/auth_token.h", "src/modules/token-auth/src/token_auth.cpp",
        "CMakeLists.txt"]


def run(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)], capture_output=True, text=True)


class SelfCheckWiringGateTest(unittest.TestCase):
    def tree(self) -> Path:
        root = Path(tempfile.mkdtemp(prefix="self-check-wiring-"))
        self.addCleanup(shutil.rmtree, root, True)
        for rel in COPY:
            target = root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / rel, target)
        return root

    def edit(self, root: Path, rel: str, old: str, new: str) -> None:
        path = root / rel
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def append(self, root: Path, rel: str, text: str) -> None:
        path = root / rel
        path.write_text(path.read_text(encoding="utf-8") + text, encoding="utf-8")

    def test_the_tree_passes(self):
        result = run(REPO)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("self-check wiring OK", result.stdout)

    def test_a_pc_unit_that_is_not_turned_on_fails(self):
        root = self.tree()
        self.edit(root, "src/runtime/compat/ws_bridge.cpp", "nevr_self_check::SetEnabled(true);", "")
        result = run(root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not turn the unit on", result.stderr)

    def test_a_conditional_enable_fails(self):
        root = self.tree()
        self.edit(root, "src/runtime/compat/ws_bridge.cpp", "static void WireSelfChecks() {\n  nevr_self_check::SetEnabled(true);",
                  "static void WireSelfChecks() {\n#ifdef SOME_SWITCH\n  nevr_self_check::SetEnabled(true);\n#endif")
        result = run(root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not turn the unit on", result.stderr)

    def test_a_build_switch_by_stamp_or_feature_fails(self):
        for token in ("NEVR_SELF_CHECKS", "nevr_self_checks_by_stamp", "Feature::kSelfCheck", "effective.selfCheck"):
            root = self.tree()
            self.append(root, "CMakeLists.txt", f"\n# {token}\n")
            result = run(root)
            self.assertEqual(result.returncode, 1, token)
            self.assertIn(token, result.stderr)

    def test_a_debug_query_by_build_type_fails(self):
        for text in ("AppendRemoteDebugParam(url);", "WantsRemoteDebug(1, false)", "bool remoteDebugQuery;",
                     'url += "?debug=true";', 'AppendQuery(uri, {{"debug", "true", false}});'):
            root = self.tree()
            self.append(root, "src/quest/integration/self_check_wiring.cpp", f"\n// {text}\n")
            result = run(root)
            self.assertEqual(result.returncode, 1, text)

    def test_a_later_disable_fails_outside_tests_only(self):
        root = self.tree()
        self.append(root, "src/quest/integration/self_check_wiring.cpp", "\nvoid X() { nevr_self_check::SetEnabled(false); }\n")
        result = run(root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("SetEnabled(false)", result.stderr)

    def test_a_party_share_check_that_is_not_fed_fails_on_either_platform(self):
        root = self.tree()
        self.edit(root, "src/runtime/compat/ws_bridge.cpp",
                  "if (fromServer) nevr_party_share_check::OnServerMessage(sym);", "")
        self.assertEqual(run(root).returncode, 1)
        root = self.tree()
        self.edit(root, "src/runtime/compat/ws_bridge.cpp",
                  "else nevr_party_share_check::OnClientMessage(sym, payload, static_cast<size_t>(len));", "")
        self.assertEqual(run(root).returncode, 1)
        root = self.tree()
        self.edit(root, "src/quest/integration/production_steps.cpp",
                  "ObserveForSelfChecks(serverToGame, data, len);", "")
        self.assertEqual(run(root).returncode, 1)
        root = self.tree()
        self.edit(root, "src/quest/integration/production_steps.cpp",
                  "ObserveForSelfChecks(false, reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());", "")
        self.assertEqual(run(root).returncode, 1)

    def test_a_status_redirect_check_that_is_not_fed_or_registered_fails(self):
        for rel, old, new in (
            ("src/runtime/lifecycle/config.cpp", "nevr_status_redirect_check::NoteConnect(gameUri, uri);", ""),
            ("src/runtime/compat/ws_bridge.cpp",
             "nevr_status_redirect_check::Register(&nevr_status_redirect_check::ProcessProbe);", ""),
            ("src/quest/redirect/service_redirector.cpp", "nevr_status_redirect_check::NoteConnect(url, url);", ""),
            ("src/quest/redirect/service_redirector.cpp", "nevr_status_redirect_check::NoteConnect(url, chosen);", ""),
            ("src/quest/integration/production_steps.cpp",
             "nevr_status_redirect_check::Register(&nevr_status_redirect_check::ProcessProbe);", ""),
        ):
            root = self.tree()
            self.edit(root, rel, old, new)
            self.assertEqual(run(root).returncode, 1, (rel, old))

    def test_a_party_leave_check_that_is_not_fed_fails(self):
        for rel, old in (
            ("src/runtime/compat/ws_bridge.cpp",
             "if (fromServer) nevr_party_leave_check::OnServerMessage(sym, payload, static_cast<size_t>(len));"),
            ("src/quest/integration/production_steps.cpp",
             "nevr_party_leave_check::OnServerMessage(message.symbol, message.payload, static_cast<std::size_t>(message.length));"),
        ):
            root = self.tree()
            self.edit(root, rel, old, "")
            self.assertEqual(run(root).returncode, 1, (rel, old))

    def test_a_friend_request_check_that_is_not_fed_fails(self):
        for rel, old in (
            ("src/runtime/compat/ws_bridge.cpp",
             "if (fromServer) nevr_friend_request_check::OnServerMessage(sym, payload, static_cast<size_t>(len));"),
            ("src/quest/integration/production_steps.cpp",
             "nevr_friend_request_check::OnServerMessage(message.symbol, message.payload, static_cast<std::size_t>(message.length));"),
        ):
            root = self.tree()
            self.edit(root, rel, old, "")
            self.assertEqual(run(root).returncode, 1, (rel, old))

    def test_a_pc_quit_check_that_is_not_called_fails(self):
        root = self.tree()
        self.edit(root, "src/runtime/lifecycle/crash_recovery.cpp", "nevr_quit_check::OnGameLoopReturned(gameResult);", "")
        self.assertEqual(run(root).returncode, 1)

    def test_a_credential_cache_check_that_is_not_fed_or_registered_fails(self):
        for rel, old in (
            ("src/runtime/compat/ws_bridge.cpp",
             "nevr_credential_cache_check::Register(&nevr_credential_cache_check::ProcessProbe);"),
            ("src/core/auth_token.h", "nevr_credential_stats::NoteSaved(protection);"),
            ("src/core/auth_token.h", "nevr_credential_stats::NoteRefused(why);"),
            ("src/modules/token-auth/src/token_auth.cpp", "nevr_credential_stats::NoteCacheReused();"),
            ("src/modules/token-auth/src/token_auth.cpp", "nevr_credential_stats::NoteSignInRequired();"),
        ):
            root = self.tree()
            self.edit(root, rel, old, "")
            if old.endswith("NoteRefused(why);"):  # SaveAuthToken refuses in two places: take both out
                self.edit(root, rel, old, "")
            self.assertEqual(run(root).returncode, 1, (rel, old))

    def test_losing_the_quest_wiring_fails(self):
        root = self.tree()
        self.edit(root, "src/quest/integration/production_steps.cpp", "ApplySelfCheck(&config.tap,", "(void)(&config.tap,")
        result = run(root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("ApplySelfCheck", result.stderr)


if __name__ == "__main__":
    unittest.main()
