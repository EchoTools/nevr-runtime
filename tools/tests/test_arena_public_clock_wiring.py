"""#982: a public Arena match's clock keeps counting through goals on a server.

Source sensors for what the GTests in src/runtime/tests/test_arena_public_clock.cpp cannot reach (they drive the
pure decisions in patch/arena_public_clock_rules.h; the hook, the observer and the boot call live in
arena_public_clock.cpp and boot.cpp, which no test target links):

  - boot.cpp installs the patch inside the server-only block, so a client never hooks the scripts;
  - the hook detours the bind function of every site in the rules header, not the node, and changes nothing but
    the state the decision is applied to;
  - a site is classified before it is hooked, and a mismatch is logged at Warning;
  - the decision log line carries applied, reason and game_type, the shape of arena_round_time's line.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def hook() -> str:
    return strip_comments(read("src/runtime/patch/arena_public_clock.cpp"))


class BootInstallsItOnAServerOnly(unittest.TestCase):
    def test_install_is_called_inside_the_server_block(self):
        boot = strip_comments(read("src/runtime/lifecycle/boot.cpp"))
        call = boot.index("nevr_arena_public_clock::Install()")
        block_start = boot.rindex("if (g_isServer) {", 0, call)
        # No block closes between the server `if` and the call: the call sits directly in that block.
        between = boot[block_start + len("if (g_isServer) {"):call]
        self.assertEqual(between.count("{"), between.count("}"), "the call must not be inside a nested block")
        self.assertNotIn("\n  }\n", between, "the server block closed before the call")

    def test_the_source_is_in_the_runtime_build_and_the_inventory(self):
        cmake = read("src/runtime/CMakeLists.txt")
        self.assertIn('"patch/arena_public_clock.cpp"', cmake)
        self.assertIn('"patch/arena_public_clock_rules.h"', cmake)
        self.assertIn("patch/arena_public_clock.cpp", read("tools/verify_patch_source_inventory.py"))


class TheHookIsTheBindFunctionOfEverySite(unittest.TestCase):
    def test_one_detour_per_site_row(self):
        code = hook()
        self.assertIn("DetourTable(std::make_index_sequence<kSiteCount>", code)
        self.assertIn("site.bindRva", code)
        self.assertNotIn("idRva", code, "the node is validated in the rules header, never hooked or written")

    def test_the_site_is_classified_before_it_is_hooked(self):
        body = extract_braced_function(hook(), "void Refresh()")
        classify = body.index("ClassifySite(")
        attach = body.index("HookSite(")
        self.assertLess(classify, attach)
        self.assertIn("SiteState::Unhooked", body)
        self.assertIn("Hooking::Attach(", hook(), "the default attach is the hooking layer's")

    def test_a_mismatch_is_a_warning_and_the_script_keeps_its_behaviour(self):
        body = extract_braced_function(hook(), "void Report(")
        self.assertIn("LogLevel::Warning", body)
        self.assertRegex(read("src/runtime/patch/arena_public_clock.cpp"), r"keeps the game's behaviour")

    def test_a_reload_removes_the_stale_hook_before_hooking_again(self):
        body = extract_braced_function(hook(), "bool HookSite(std::size_t i, void* target)")
        self.assertLess(body.index("g_backend.remove("), body.index("g_backend.attach("))
        self.assertIn("MH_RemoveHook(target)", hook(), "the default removal is MinHook's")

    def test_the_trampoline_is_published_into_the_slot_the_detour_reads(self):
        body = extract_braced_function(hook(), "bool HookSite(std::size_t i, void* target)")
        self.assertIn("g_backend.attach(&hook.original,", body, "a stack local would leave the detour a null until the store after Attach")
        self.assertLess(body.index("__atomic_store_n(&hook.original, target"), body.index("g_backend.attach("))


class TheDecisionIsLogged(unittest.TestCase):
    def test_the_line_has_applied_reason_and_game_type(self):
        code = read("src/runtime/patch/arena_public_clock.cpp")
        m = re.search(r'"\[NEVR\.PATCH\] arena_public_clock applied=%d reason=%s game_type=%s', code)
        self.assertIsNotNone(m, "the decision line must carry applied=, reason= and game_type=")

    def test_the_other_flags_log_nothing(self):
        body = extract_braced_function(hook(), "std::int64_t BindDetour(void* state, void* node)")
        # The Log call is inside the id check; the pass-through for every other flag is the final return.
        self.assertLess(body.index("if (id && *id == kPublicMatchFlag)"), body.index("Log("))


if __name__ == "__main__":
    unittest.main()
