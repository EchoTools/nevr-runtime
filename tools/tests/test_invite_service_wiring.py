"""#1156: the invite service is started, fed and stopped by the lines the GTests cannot reach.

The service (patch/invite_service.cpp) is tested by test_invite_service against a loopback server. What no test target
links is boot.cpp, the wizard hook and the two shutdown paths, so the shape of those lines is pinned here:

  - boot starts the service on a client, from the config key and the service address, before the observer installs, and
    says on a server that it skipped;
  - the token source is the thread-safe nevr_token_auth::GetToken, never the module export (a static buffer);
  - the wizard hook hands a typed code and a create to the service (the answer is shown by test_invite_answer_wiring);
  - the client's normal exit and the DLL unload both stop the service.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments


ROOT = Path(__file__).resolve().parents[2]


def code(path: str) -> str:
    return strip_comments((ROOT / path).read_text())


class BootStartsTheService(unittest.TestCase):
    def test_the_service_starts_from_the_config_key_and_the_service_address(self):
        boot = code("src/runtime/lifecycle/boot.cpp")
        self.assertRegex(boot, r'ParseSwitch\(\s*NevrCfgGetFlat\(\s*"nevr_invite_codes"\s*\)\s*\)')
        self.assertIn('NevrCfgGetFlat("nevr_http_uri")', boot)
        self.assertIn("nevr_invite_service::Start(std::move(invite))", boot)

    def test_the_service_starts_before_the_observer_is_installed(self):
        boot = code("src/runtime/lifecycle/boot.cpp")
        self.assertLess(boot.index("nevr_invite_service::Start("), boot.index("nevr_invite_hook::Install("))

    def test_the_token_comes_from_the_thread_safe_function_not_the_export(self):
        boot = code("src/runtime/lifecycle/boot.cpp")
        self.assertRegex(boot, r"invite\.token\s*=\s*\[\]\s*\{\s*return\s+nevr_token_auth::GetToken\(\)\s*;\s*\}")

    def test_a_server_says_it_skipped(self):
        self.assertIn("[NEVR.INVITE] service skipped reason=server_mode", (ROOT / "src/runtime/lifecycle/boot.cpp").read_text())


class TheHookFeedsTheService(unittest.TestCase):
    def test_a_resolve_and_a_create_are_both_submitted(self):
        hook = code("src/runtime/patch/invite_wizard_hook.cpp")
        # The one Submit sits after the "not sent" return and is not conditional on the operation.
        self.assertEqual(len(re.findall(r"nevr_invite_service::Submit\(\s*plan\s*\)\s*;", hook)), 1)
        self.assertNotRegex(hook, r"if\s*\(\s*plan\.op\s*==\s*nevr_invite::Op::(Resolve|Create)\s*\)")
        self.assertLess(hook.index("NotSentLine(plan.skip"), hook.index("nevr_invite_service::Submit(plan)"))
        self.assertNotIn("PlannedLine", hook)


class ThePlanNamesTheMapCardsLevel(unittest.TestCase):
    """A map card's level is named through the symbol cache; a plan built without that lookup refuses every payload and
    capture-point create as no_level, so both callers of PlanRequest pass it."""

    def test_the_wizard_hook_passes_the_symbol_cache_lookup(self):
        hook = code("src/runtime/patch/invite_wizard_hook.cpp")
        self.assertRegex(hook, r"nevr_invite::PlanRequest\(\s*metric\s*,\s*choices\s*,\s*LevelName\s*\)")
        self.assertRegex(hook, r"NameForHash\(\s*evr::kSymCacheData\s*,[^;]*evr::kSymCacheCount")

    def test_the_scenario_create_passes_the_same_lookup(self):
        scenario = code("src/runtime/scenario/scenario_control.cpp")
        self.assertRegex(scenario, r"nevr_invite::PlanRequest\(\s*metric\s*,\s*choices\s*,\s*nevr_invite_hook::LevelName\s*\)")


class ShutdownStopsTheService(unittest.TestCase):
    def test_the_clients_normal_exit_stops_it_after_the_cdn_thread(self):
        text = code("src/runtime/lifecycle/crash_recovery.cpp")
        self.assertIn("nevr_invite_service::Shutdown()", text)
        self.assertLess(text.index("nevr_asset_cdn::StopBackgroundFetch()"), text.index("nevr_invite_service::Shutdown()"))

    def test_the_dll_unload_stops_it(self):
        self.assertIn("nevr_invite_service::Shutdown()", code("src/runtime/lifecycle/dllmain.cpp"))


if __name__ == "__main__":
    unittest.main()
