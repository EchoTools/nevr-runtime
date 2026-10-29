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
        self.assertIn('NevrCfgGetFlat("nevr_invite_codes")', boot)
        # A list or a map under the key is read as unrecognised, never as the absent key that takes the default (#1156).
        self.assertIn('NevrCfgGetFlatIsStructured("nevr_invite_codes")', boot)
        self.assertRegex(boot, r"invite\.invite_codes\s*=\s*nevr_invite::ParseSwitch\(\s*inviteText\s*,\s*inviteStructured\s*\)")
        self.assertRegex(boot, r"invite\.invite_source\s*=\s*nevr_invite::SourceOf\(\s*inviteText\s*,\s*inviteStructured\s*\)")
        self.assertIn('NevrCfgGetFlat("nevr_http_uri")', boot)
        self.assertIn("nevr_invite_service::Start(std::move(invite))", boot)

    def test_the_start_is_in_the_client_branch_so_a_server_never_starts_it(self):
        boot = code("src/runtime/lifecycle/boot.cpp")
        settings = boot.index("nevr_invite_service::Settings invite;")
        start = boot.index("nevr_invite_service::Start(std::move(invite))")
        # The block opens right before its settings. A search further back would find the menu-cards guard above it.
        self.assertIn("if (!g_isServer) {", boot[settings - 40:settings])
        self.assertLess(settings, start)
        self.assertLess(start, boot.index("} else {", start))  # the server branch comes after the client's start

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


class TheDocsStateTheDefault(unittest.TestCase):
    """ADR 0017 and the example config say the key is on when absent, so a reader of either gets the default right."""

    def test_the_example_config_says_absent_is_on(self):
        text = (ROOT / "docs/reference/example-config.yaml").read_text()
        block = text[text.index("The invite codes for the CREATE GAME"):text.index("# invite_codes: false")]
        self.assertIn("# Absent: on.", block)
        self.assertNotIn("Absent: off.", block)

    def test_the_adr_says_the_feature_is_on_by_default(self):
        adr = (ROOT / "docs/adr/0017-invite-codes-for-the-create-game-wizard.md").read_text()
        self.assertIn("is on by default for a client", adr)
        self.assertNotIn("turns it on (N133)", adr)

    def test_the_adr_log_table_names_the_source(self):
        adr = (ROOT / "docs/adr/0017-invite-codes-for-the-create-game-wizard.md").read_text()
        self.assertIn("| service on | Info | `source=default\\|config`", adr)
        self.assertIn("| feature off | Info / Warning | `reason=turned_off`", adr)
        self.assertIn("`source=default\\|config`", adr.split("| feature off |", 1)[1].split("\n", 1)[0])


class TheQuickCheckRunsTheInviteTests(unittest.TestCase):
    """The GTests are built by CMake; only the recipe that names them runs them. test-auth-unit builds and runs both."""

    def test_test_auth_unit_builds_and_runs_the_service_and_the_worker(self):
        text = (ROOT / "justfile").read_text()
        recipe = text[text.index("test-auth-unit:"):text.index("test-auth-integration:")]
        loop = [line for line in recipe.splitlines() if "for test_name in" in line]
        self.assertEqual(len(loop), 1)
        for name in ("test_invite_service", "test_invite_worker"):
            self.assertIn(f"--target {name}", recipe)
            self.assertIn(name, loop[0].split())


if __name__ == "__main__":
    unittest.main()
