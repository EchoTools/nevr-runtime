"""#697: the command-line flags are kebab-case; the names they had are hidden deprecated aliases.

The alias table is src/core/cli_flags.h. These checks read the sources that must agree with it:
cli.cpp (what the game's parser is told and what --help lists), boot.cpp (what the parse reads), and every
tracked file (no old name outside the files that implement or test the aliases).
"""
import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8")


def flag_table():
    """[(flag, alias or None)] from src/core/cli_flags.h's kFlags."""
    text = read("src/core/cli_flags.h")
    body = text[text.index("inline constexpr FlagSpec kFlags[]"):]
    body = body[: body.index("};")]
    rows = re.findall(r'\{L"(-[a-z-]+)",\s*(?:L"(-[a-z-]+)"|nullptr)\}', body)
    return [(flag, alias or None) for flag, alias in rows]


ALIASES = {
    "-noconsole": "-no-console",
    "-noexitonerror": "-no-exit-on-error",
    "-exitonerror": "-exit-on-error",
    "-notelemetry": "-no-telemetry",
    "-telemetryrate": "-telemetry-rate",
    "-telemetrydiag": "-telemetry-diag",
    "-traceexports": "-trace-exports",
}

# Files that implement or pin the aliases, or quote output captured from another program. Every other tracked
# file may not carry an old name.
MAY_NAME_ALIASES = {
    "src/core/cli_flags.h",
    "src/runtime/lifecycle/cli.cpp",
    "src/runtime/hook/export_trace_policy.h",
    "src/runtime/tests/test_cli_check.cpp",
    "src/runtime/tests/test_cmdline_strip.cpp",
    "src/runtime/tests/test_export_trace.cpp",
    "tools/tests/test_cli_flag_names.py",
    "tools/tests/test_winvm_checks.py",
}
MAY_NAME_PREFIXES = ("tools/tests/fixtures/",)


class FlagTable(unittest.TestCase):
    def test_the_table_holds_exactly_the_seven_renames(self):
        got = {alias: flag for flag, alias in flag_table() if alias}
        self.assertEqual(got, ALIASES)

    def test_every_flag_boot_reads_is_in_the_table_and_the_other_way(self):
        boot = read("src/runtime/lifecycle/boot.cpp")
        start = boot.index("  // Parse command line arguments.")
        main_parse = boot[start: boot.index("LogTimestepArgs();", start)]
        read_by_parse = set(re.findall(r'lstrcmpW\(flag, L"(-[a-z-]+)"\)', main_parse))
        table = {flag for flag, _ in flag_table()}
        self.assertEqual(read_by_parse, table, "boot.cpp's parse and kFlags disagree")

    def test_boot_reads_flags_only_through_resolve(self):
        boot = read("src/runtime/lifecycle/boot.cpp")
        self.assertEqual(re.findall(r'lstrcmpW\((?:arg|argv\[i\]), L"-', boot), [],
                         "an exact comparison of a raw token reads only one spelling (#697)")
        self.assertIn("nevr_cli::Resolve(arg)", boot)
        self.assertIn("NoteCommandLineToken(arg, named)", boot)
        note = boot[boot.index("static void NoteCommandLineToken"):boot.index("static void LogTimestepArgs")]
        self.assertIn("s_reports.First(named)", note, "an alias warns once per run (nevr_cli::AliasReports)")
        self.assertIn("nevr_cli::kAliasWarningFormat", note)
        self.assertIn("named.rejected", note)
        self.assertIn("more than two leading dashes", note, "three dashes are refused with a Warning")


class GameParserRegistration(unittest.TestCase):
    def setUp(self):
        self.cli = read("src/runtime/lifecycle/cli.cpp")
        self.syntax = set(re.findall(r'AddArgSyntax\(pArgSyntax, "(-[a-z-]+)"', self.cli))
        self.helped = set(re.findall(r'AddArgHelpString\(pArgSyntax,\s*"(-[a-z-]+)"', self.cli))

    def test_each_alias_is_registered_so_the_game_accepts_it_and_has_no_help_line(self):
        for alias, flag in ALIASES.items():
            self.assertIn(alias, self.syntax, f"{alias}: the game rejects a token with no syntax entry")
            self.assertNotIn(alias, self.helped, f"{alias}: --help must not list a deprecated name")
            self.assertIn(flag, self.syntax)
            self.assertIn(flag, self.helped, f"{flag}: the new name is documented")

    def test_help_text_holds_no_old_name(self):
        for line in self.cli.splitlines():
            if "AddArgSyntax" in line:
                continue
            for alias in ALIASES:
                self.assertIsNone(re.search(r"(?<![\w-])" + re.escape(alias) + r"\b", line), line)

    def test_the_syntax_arity_of_an_alias_matches_its_new_flag(self):
        arity = dict(re.findall(r'AddArgSyntax\(pArgSyntax, "(-[a-z-]+)", (\d, \d)', self.cli))
        for alias, flag in ALIASES.items():
            self.assertEqual(arity[alias], arity[flag], f"{alias} and {flag} take the same values")


class DbgcoreFlagsRemoved(unittest.TestCase):
    """#875: -skip-dbgcore-check and -allow-dbgcore are gone from the table, the parse and the globals.

    They stay registered in cli.cpp with no help line, so the game's parser accepts them and the unconditional
    dbgcore.dll check refuses the start on any command line.
    """
    REMOVED = ("-skip-dbgcore-check", "-allow-dbgcore")

    def setUp(self):
        self.cli = read("src/runtime/lifecycle/cli.cpp")
        self.syntax = set(re.findall(r'AddArgSyntax\(pArgSyntax, "(-[a-z-]+)"', self.cli))
        self.helped = set(re.findall(r'AddArgHelpString\(pArgSyntax,\s*"(-[a-z-]+)"', self.cli))

    def test_the_table_and_the_parse_do_not_name_them(self):
        table = {flag for flag, _ in flag_table()} | {alias for _, alias in flag_table() if alias}
        for name in self.REMOVED:
            self.assertNotIn(name, table)
        boot = read("src/runtime/lifecycle/boot.cpp")
        for name in self.REMOVED:
            self.assertNotIn(f'L"{name}"', boot)

    def test_no_global_or_skip_branch_in_boot_or_globals(self):
        for rel in ("src/core/globals.h", "src/core/globals.cpp", "src/runtime/lifecycle/boot.cpp"):
            self.assertNotIn("g_allowDbgCore", read(rel), rel)

    def test_both_names_stay_registered_without_help(self):
        for name in self.REMOVED:
            self.assertIn(name, self.syntax, f"{name}: the game rejects a token with no syntax entry")
            self.assertNotIn(name, self.helped, f"{name}: --help must not list a removed name")


class NoOldNamesElsewhere(unittest.TestCase):
    def test_no_tracked_file_outside_the_alias_code_carries_an_old_name(self):
        files = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True, check=True).stdout.split("\n")
        pattern = re.compile(r"(?<![\w-])-(?:" + "|".join(re.escape(a[1:]) for a in ALIASES) + r")\b")
        offenders = []
        for rel in files:
            if not rel or rel in MAY_NAME_ALIASES or rel.startswith(MAY_NAME_PREFIXES):
                continue
            try:
                text = (REPO / rel).read_text(encoding="utf-8")
            except (UnicodeDecodeError, FileNotFoundError, IsADirectoryError):
                continue
            for number, line in enumerate(text.splitlines(), 1):
                if pattern.search(line):
                    offenders.append(f"{rel}:{number}: {line.strip()[:100]}")
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
