"""#596: g_plugins has stable element addresses by construction, not by a reserve that predicts a
later pass's count; the early-plugin latch in the boot hook is atomic like its neighbours; N146
says what the early pass does.  Source-shape sensors; the runtime behaviour (a kept
GetLoadedPluginInfo pointer survives further loads) is src/runtime/tests/test_behavioral.cpp."""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]


def loader_source() -> str:
    return (ROOT / "src/runtime/ext/plugin_loader.cpp").read_text()


class PluginStorage(unittest.TestCase):
    def test_g_plugins_is_a_container_whose_elements_do_not_move(self):
        self.assertRegex(loader_source(), r"static\s+std::deque<LoadedPlugin>\s+g_plugins\s*;")

    def test_no_reserve_predicts_a_later_pass(self):
        source = loader_source()
        self.assertNotIn("laterCount", source)
        self.assertNotRegex(source, r"g_plugins\.reserve\(")


class EarlyPassLatch(unittest.TestCase):
    def test_early_plugin_latch_is_atomic_and_claimed_once(self):
        source = (ROOT / "src/runtime/lifecycle/boot.cpp").read_text()
        body = extract_braced_function(source, "UINT64 PreprocessCommandLineHook(PVOID pGame)")
        self.assertRegex(body, r"static\s+std::atomic_bool\s+s_earlyPlugins\s*\{\s*false\s*\}\s*;")
        self.assertRegex(
            body,
            r"if\s*\(\s*!\s*s_earlyPlugins\.exchange\(\s*true\b[^)]*\)\s*\)\s*\{\s*LoadPlugins\(PluginPhase::Early\)\s*;",
        )


class N146Row(unittest.TestCase):
    def test_n146_describes_the_early_plugin_pass(self):
        rows = [l for l in (ROOT / "AGENTS.md").read_text().splitlines() if l.startswith("| N146 |")]
        self.assertEqual(len(rows), 1)
        rule = rows[0].split("|")[3]  # | Rule | Kind | Statement | Guards | Checked by |
        self.assertRegex(rule, r"early")
        self.assertIn("PluginPhase::Early", rule)
        self.assertNotRegex(rule, r"only minimal state is set up \(no modules, plugins")


if __name__ == "__main__":
    unittest.main()
