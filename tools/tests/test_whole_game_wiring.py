"""#2398: the whole-game launch's first-run download and mount are wired where the data step reaches them.

src/quest/tests/whole_game_launch_test.cpp drives the data step with the worker the sentinel sets (nevr_quest::cargo::MakeFirstRunWorker).
The line that sets it lives in src/quest/sentinel/entry.cpp, which no host test links, so it is checked here over comment-stripped source:

  - InstallPermissionWaitHook sets options.all_assets_ready, from the production worker over the configuration's assets, and does so
    before data_wiring::Install hands the options over;
  - the mode and the worker are set from the same configuration (ActiveConfig().assets);
  - the sentinel's cargo library builds the first-run prefetch the worker runs through;
  - the host test target for the launch exists.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def code(path: str) -> str:
    return strip_comments(read(path))


class WholeGameWiring(unittest.TestCase):
    def setUp(self) -> None:
        self.body = extract_braced_function(code("src/quest/sentinel/entry.cpp"), "InstallPermissionWaitHook")

    def test_the_worker_is_set_before_the_options_are_installed(self) -> None:
        ready = self.body.index("options.all_assets_ready")
        install = self.body.index("nevr_quest::data_wiring::Install(options)")
        self.assertLess(ready, install)

    def test_the_worker_is_the_production_first_run_worker(self) -> None:
        self.assertRegex(
            self.body,
            r"options\.all_assets_ready\s*=\s*nevr_quest::cargo::MakeProductionFirstRunWorker\(\s*sentinel::ActiveConfig\(\)\.assets\s*\)",
        )

    def test_the_mode_and_the_worker_come_from_the_same_configuration(self) -> None:
        mode = re.search(r"options\.all_assets\s*=\s*(sentinel::ActiveConfig\(\)\.assets)\.wholeGame", self.body)
        self.assertIsNotNone(mode)

    def test_no_known_wrong_marker_for_2398_remains(self) -> None:
        for path in ("src/quest/sentinel/entry.cpp", "docs/adr/0045-cdn-texture-delivery.md"):
            self.assertNotIn("KNOWN-WRONG #2398", read(path), path)

    def test_the_sentinel_builds_the_prefetch_the_worker_runs_through(self) -> None:
        cmake = read("src/quest/sentinel/CMakeLists.txt")
        self.assertRegex(cmake, r"add_library\(nevr_quest_cargo STATIC[^)]*core/cargo_prefetch\.cpp")

    def test_the_launch_has_a_host_test_target(self) -> None:
        cmake = read("src/quest/host_tests/groups/data.cmake")
        self.assertIn("nevr_ht_test(whole_game_launch_test", cmake)


if __name__ == "__main__":
    unittest.main()
