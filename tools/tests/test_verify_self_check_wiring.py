"""tools/verify_self_check_wiring.py: the self-check wiring gate passes on the tree and fails on each mutant (#451)."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "verify_self_check_wiring.py"
FILES = [
    "src/runtime/compat/ws_bridge.cpp",
    "src/quest/integration/production_steps.cpp",
    "src/runtime/compat/self_check.h",
]


def run(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)], capture_output=True, text=True)


class SelfCheckWiringGateTest(unittest.TestCase):
    def tree(self) -> Path:
        root = Path(tempfile.mkdtemp(prefix="self-check-wiring-"))
        self.addCleanup(shutil.rmtree, root, True)
        for rel in FILES:
            target = root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / rel, target)
        return root

    def mutate(self, root: Path, rel: str, old: str, new: str) -> None:
        path = root / rel
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def test_the_tree_passes(self):
        result = run(REPO)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("self-check wiring OK", result.stdout)

    def test_a_gate_that_ignores_the_connection_or_the_server_flag_fails(self):
        root = self.tree()
        self.mutate(root, "src/runtime/compat/ws_bridge.cpp",
                    "nevr_self_check::WantsRemoteDebug(connIdx, g_isServer != FALSE)", "true")
        result = run(root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("WantsRemoteDebug(connIdx, g_isServer)", result.stderr)

    def test_a_widened_gate_function_fails(self):
        root = self.tree()
        self.mutate(root, "src/runtime/compat/self_check.h", "connIdx == 1 && !isServer && Enabled()", "Enabled()")
        self.assertEqual(run(root).returncode, 1)

    def test_losing_the_quest_wiring_or_the_pc_sender_fails(self):
        root = self.tree()
        self.mutate(root, "src/quest/integration/production_steps.cpp", "ApplySelfCheck(&config.tap,", "(void)(&config.tap,")
        self.assertEqual(run(root).returncode, 1)
        root = self.tree()
        self.mutate(root, "src/runtime/compat/ws_bridge.cpp", "  WireSelfChecks();\n", "")
        self.assertEqual(run(root).returncode, 1)


if __name__ == "__main__":
    unittest.main()
