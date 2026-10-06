"""VM scenario routing and environment-failure contracts."""

from __future__ import annotations

import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "winvm"))
import systest  # noqa: E402


class ScenarioDispatchTest(unittest.TestCase):
    def test_default_all_dispatches_gai_then_boot(self):
        calls = []

        def gai_runner(*args):
            calls.append("gai")
            return []

        def boot_runner(*args, **kwargs):
            calls.append("boot")
            return []

        self.assertEqual(systest.scenario_plan("all"), ("gai", "boot"))
        systest.dispatch_scenarios("all", object(), pathlib.Path("out"), pathlib.Path("runtime.dll"),
                                  object(), gai_runner, boot_runner)
        self.assertEqual(calls, ["gai", "boot"])

    def test_explicit_boot_and_gai_dispatch_only_the_selected_check(self):
        for scenario in ("boot", "gai"):
            with self.subTest(scenario=scenario):
                calls = []

                def gai_runner(*args):
                    calls.append("gai")
                    return []

                def boot_runner(*args, **kwargs):
                    calls.append("boot")
                    return []

                systest.dispatch_scenarios(
                    scenario, object(), pathlib.Path("out"), pathlib.Path("runtime.dll"),
                    object(), gai_runner, boot_runner)
                self.assertEqual(calls, [scenario])


if __name__ == "__main__":
    unittest.main()
