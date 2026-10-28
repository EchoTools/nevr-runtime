"""#1137: the login-failed page's title hook is wired from the sentinel to the game.

The host tests drive the handler through the thunk (login_title_hook_test), the post-load policy with fakes
(integration_sequence_test) and the slot against the real library (got_pinned_test). What none of them can see is
that production binds those pieces together, so that is pinned here by source shape.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


class TheTitleHookIsWired(unittest.TestCase):
    def test_the_slot_is_pinned_by_module_symbol_and_address(self):
        text = read("src/quest/sentinel/pinned_targets.h")
        self.assertIn('kLoginScriptLib = "lib7f2363b8976491aa.so"', text)
        self.assertIn('kStringTCtorSymbol = "_ZN8string_tC2EPKc"', text)
        body = extract_braced_function(text, "inline GotTarget LoginScriptStringCtor() {")
        self.assertRegex(body, r"kLoginScriptLib,\s*kStringTCtorSymbol,\s*RelocKind::kJumpSlot,\s*nullptr,\s*0x12dd0ULL")

    def test_the_literal_the_handler_replaces_is_the_games_title(self):
        self.assertIn('kGameTitle[] = "LOGIN FAILED"', read("src/quest/sentinel/login_title_hook.h"))
        body = extract_braced_function(
            read("src/quest/sentinel/login_title_hook.cpp"),
            "void StringHandler(StringThunk::Fn original, sentinel::pinned::StringTOpaque* self, const char* text) noexcept {",
        )
        self.assertIn("std::strcmp(text, kGameTitle) != 0", body)
        self.assertIn("board::ReadTitle(title, sizeof(title))", body)

    def test_the_hook_is_built_without_exceptions_into_the_sentinel(self):
        cmake = read("src/quest/sentinel/CMakeLists.txt")
        self.assertRegex(cmake, r"add_library\(ovrplatformloader SHARED[^)]*\blogin_title_hook\.cpp")
        props = re.search(r"set_source_files_properties\(([^)]*)PROPERTIES COMPILE_OPTIONS \"-fno-exceptions\"\)", cmake)
        self.assertIsNotNone(props)
        self.assertIn("login_title_hook.cpp", props.group(1))

    def test_its_counters_register_in_the_login_prompt_step_so_the_reporter_holds_them(self):
        body = extract_braced_function(read("src/quest/sentinel/entry.cpp"), "bool RegisterLoginPromptCounters() noexcept {")
        self.assertIn("nevr_quest::login_prompt::RegisterCounters()", body)
        self.assertIn("nevr_quest::login_title::RegisterCounters()", body)

    def test_the_install_is_a_post_load_action_wanted_with_the_login(self):
        steps = read("src/quest/integration/production_steps.cpp")
        body = extract_braced_function(steps, "bool InstallDlopenHook(bool login, bool matchmaking, bool plugins) override {")
        self.assertRegex(body, r"if \(login\) actions\.loginTitle = &LoginTitleAction;")
        action = extract_braced_function(steps, "ActionResult LoginTitleAction() noexcept {")
        self.assertIn("nevr_quest::integration::InstallLoginTitleHook()", action)
        self.assertIn("Settle::kRetryLater, \"module_not_loaded\"", action)
        seam = extract_braced_function(
            read("src/quest/sentinel/entry.cpp"), "sentinel::GotStatus InstallLoginTitleHook() noexcept {"
        )
        self.assertIn("nevr_quest::login_title::Install()", seam)

    def test_the_post_load_policy_runs_the_title_slot_after_every_dlopen_until_it_settles(self):
        cpp = read("src/quest/integration/post_load.cpp")
        self.assertIn("RunSlot(S().loginTitle, module);", cpp)
        self.assertIn("S().loginTitle.fn != nullptr && !S().loginTitle.settled", cpp)


class TheTitlesComeFromTheSamePlaceAsTheCodeText(unittest.TestCase):
    def test_the_presenter_publishes_each_text_with_its_title(self):
        cpp = read("src/quest/auth/prompt_presenters.cpp")
        self.assertIn('Publish(text, prompt_board::Mode::kPrompt, "code", kSignInTitle)', cpp)
        self.assertIn('Publish(kSignedInText, prompt_board::Mode::kNotice, "signed_in", kSignedInTitle)', cpp)
        # A failure publishes no title, so the game's own LOGIN FAILED stays over it.
        self.assertIn('Publish(kTimedOutText, prompt_board::Mode::kPrompt, "timed_out", nullptr)', cpp)
        self.assertIn('Publish(kNoCodeShownText, prompt_board::Mode::kPrompt, "no_code_shown", nullptr)', cpp)
        header = read("src/quest/auth/prompt_presenters.h")
        self.assertIn('kSignInTitle[] = "SIGN IN"', header)
        self.assertIn('kSignedInTitle[] = "SIGNED IN"', header)


if __name__ == "__main__":
    unittest.main()
