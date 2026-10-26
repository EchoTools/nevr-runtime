"""#924: the plugin trust mode of a run comes from the command line, read before the first plugin pass.

The behaviour is pinned by PluginRunModeTest in src/runtime/tests/test_behavioral.cpp (a real LoadPlugins over an
unsigned plugin, with and without -require-signed-plugins). These checks read the sources for what a GTest cannot
reach: the boot code that feeds the command line to the mode, the order against the early plugin pass, the game
parser's registration of the argument, and the README listing it.
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    """The text of the brace-balanced body that follows `signature`."""
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for index in range(open_brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:index + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


def code(text: str) -> str:
    """Comments removed, so prose cannot satisfy a check."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class ModeIsReadBeforeTheFirstPluginPass(unittest.TestCase):
    def setUp(self):
        self.boot = read("src/runtime/lifecycle/boot.cpp")

    def test_the_preflight_gives_the_mode_both_command_line_readings(self):
        # The game tokenizes GetCommandLineA() its own way, CommandLineToArgvW differs on quotes; the mode takes both.
        body = code(function_body(self.boot, "void PreflightRuntimeBootstrap()"))
        self.assertIn("nevr_plugin_trust::ApplyCommandLine(GetCommandLineW(), GetCommandLineA())", body)

    def test_the_mode_reads_the_line_with_the_games_own_tokenizer_and_name_rule(self):
        trust = code(read("src/runtime/ext/plugin_trust.cpp"))
        self.assertIn('#include "runtime/lifecycle/cmdline_strip.h"', read("src/runtime/ext/plugin_trust.cpp"))
        self.assertIn("nevr_cmdline::Tokenize(", trust)
        self.assertIn("nevr_cmdline::detail::NameIs(", trust)
        self.assertIn("CommandLineToArgvW(", trust)
        for own in ("tolower", "towlower", "_stricmp", "lstrcmpiW", "wcsicmp"):
            self.assertNotIn(own, trust, "the name rule is cmdline_strip.h's; do not write a second one")

    def test_a_throw_while_reading_the_line_makes_the_run_signed_only_and_nothing_is_swallowed(self):
        trust = code(read("src/runtime/ext/plugin_trust.cpp"))
        body = trust[trust.index("void ApplyCommandLine("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("catch (const std::exception&)", body)
        self.assertNotIn("catch (...)", trust, "the addendum forbids an ellipsis catch")
        handler = body[body.index("catch (const std::exception&)"):]
        handler = handler[:handler.index("return;")]
        self.assertIn("Mode::SignedOnly", handler)
        self.assertIn("ModeSource::CommandLineReadFailed", handler)

    def test_the_argv_buffer_is_owned_so_no_path_leaks_it(self):
        trust = code(read("src/runtime/ext/plugin_trust.cpp"))
        self.assertIn("using ArgvPtr = std::unique_ptr<wchar_t*, ArgvDeleter>;", trust)
        self.assertEqual(trust.count("LocalFree("), 1, "one owner frees the CommandLineToArgvW buffer")
        self.assertNotIn("LocalFree(argv)", trust.replace("LocalFree(argv) == nullptr", ""))

    def test_either_reading_finding_the_flag_makes_the_run_signed_only(self):
        trust = code(read("src/runtime/ext/plugin_trust.cpp"))
        self.assertIn("game = GameReading(ansiLine);", trust)
        self.assertIn("argv = ArgvReading(wideLine);", trust)
        self.assertIn("game == Reading::Found || argv == Reading::Found", trust)
        self.assertIn("ApplyUnreadableCommandLine();", trust[trust.index("void ApplyCommandLine("):])

    def test_the_preflight_runs_before_the_early_plugin_pass(self):
        body = code(function_body(self.boot, "UINT64 PreprocessCommandLineHook(PVOID pGame)"))
        self.assertLess(body.index("PreflightRuntimeBootstrap();"), body.index("LoadPlugins(PluginPhase::Early)"))

    def test_nothing_sets_the_mode_a_second_time(self):
        # Both passes and the late-load filter read nevr_plugin_trust::ActiveMode(); only the preflight sets it.
        self.assertEqual(code(self.boot).count("nevr_plugin_trust::ApplyCommandLine("), 1)
        for rel in ("src/runtime/ext/plugin_loader.cpp", "src/runtime/hook/dll_load_hook.cpp"):
            self.assertNotIn("ApplyCommandLine(", code(read(rel)), f"{rel} sets the mode")

    def test_the_mode_is_not_read_from_the_environment_or_a_config_key(self):
        trust = code(read("src/runtime/ext/plugin_trust.cpp"))
        for forbidden in ("getenv", "GetEnvironmentVariable", "config_get", "NevrCfg"):
            self.assertNotIn(forbidden, trust)

    def test_nothing_reads_a_build_time_mode_any_more(self):
        for rel in ("src/runtime/ext/plugin_loader.cpp", "src/runtime/ext/plugin_trust.cpp", "src/runtime/ext/plugin_trust.h"):
            self.assertNotIn("BuildMode", read(rel), rel)
            self.assertNotIn("kBuildMode", read(rel), rel)


class TheArgumentIsDeclaredAndDocumented(unittest.TestCase):
    FLAG = "-require-signed-plugins"

    def test_the_game_parser_is_told_about_it_with_a_help_line(self):
        cli = read("src/runtime/lifecycle/cli.cpp")
        self.assertRegex(cli, rf'AddArgSyntax\(pArgSyntax, "{self.FLAG}", 0, 0, FALSE\)')
        self.assertRegex(cli, rf'AddArgHelpString\(pArgSyntax,\s*"{self.FLAG}"')

    def test_the_readme_lists_every_argument_the_help_text_documents(self):
        cli = read("src/runtime/lifecycle/cli.cpp")
        helped = re.findall(r'AddArgHelpString\(pArgSyntax,\s*"(-[a-z-]+)"', cli)
        self.assertIn(self.FLAG, helped)
        readme = read("README.md")
        missing = [flag for flag in helped if f"`{flag}`" not in readme]
        self.assertEqual(missing, [], "README.md must list each argument cli.cpp documents")

    def test_the_log_line_states_mode_source_and_build(self):
        loader = code(read("src/runtime/ext/plugin_loader.cpp"))
        self.assertIn('"[NEVR.PLUGIN] plugin trust: mode=%s source=%s build=%s literal=\\"%s\\"', loader)


if __name__ == "__main__":
    unittest.main()
