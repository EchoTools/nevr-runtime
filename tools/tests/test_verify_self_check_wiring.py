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
        "src/quest/integration/self_check_wiring.cpp", "src/quest/sentinel/quest_config.h",
        "src/runtime/compat/self_check.h", "CMakeLists.txt"]


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
        self.edit(root, "src/quest/integration/production_steps.cpp",
                  "if (serverToGame) ObserveForSelfChecks(data, len);", "")
        self.assertEqual(run(root).returncode, 1)

    def test_losing_the_quest_wiring_fails(self):
        root = self.tree()
        self.edit(root, "src/quest/integration/production_steps.cpp", "ApplySelfCheck(&config.tap,", "(void)(&config.tap,")
        result = run(root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("ApplySelfCheck", result.stderr)


if __name__ == "__main__":
    unittest.main()
