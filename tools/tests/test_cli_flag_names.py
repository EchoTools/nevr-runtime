"""#697: the command-line flags are kebab-case. #698: the pre-#697 spellings were removed in v6.0.0.

The flag table is src/core/cli_flags.h. These checks read the sources that must agree with it:
cli.cpp (what the game's parser is told and what --help lists), boot.cpp (what the parse reads), and every
tracked file (no removed name outside the tests that pin its rejection, the legacy code and the fixtures).
"""
import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]

# The spellings NEVR read before #697, removed in v6.0.0 (#698). Each must now be an unknown option.
REMOVED = {
    "-noconsole",
    "-noexitonerror",
    "-exitonerror",
    "-notelemetry",
    "-telemetryrate",
    "-telemetrydiag",
    "-traceexports",
    "-allow-dbgcore",
}

# The kebab-case names those spellings were renamed to: each is registered with the game and listed in --help.
RENAMED_TO = {
    "-no-console",
    "-no-exit-on-error",
    "-exit-on-error",
    "-no-telemetry",
    "-telemetry-rate",
    "-telemetry-diag",
    "-trace-exports",
    "-skip-dbgcore-check",
}

# Files that pin the rejection of a removed name, or quote output captured from another program. Every other
# tracked file may not carry a removed name.
MAY_NAME_REMOVED = {
    "src/runtime/tests/test_cmdline_strip.cpp",
    "src/runtime/tests/test_export_trace.cpp",
    "tools/tests/test_cli_flag_names.py",
    "tools/tests/test_winvm_checks.py",
}
MAY_NAME_PREFIXES = ("src/legacy/", "tools/tests/fixtures/")


def read(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8")


def flag_table():
    """The flags from src/core/cli_flags.h's kFlags, by their canonical spelling."""
    text = read("src/core/cli_flags.h")
    body = text[text.index("inline constexpr const wchar_t* kFlags[]"):]
    body = body[: body.index("};")]
    return re.findall(r'L"(-[a-z-]+)"', body)


class FlagTable(unittest.TestCase):
    def test_the_table_holds_no_removed_name(self):
        self.assertEqual(set(flag_table()) & REMOVED, set())

    def test_the_table_holds_the_renamed_flags_once(self):
        table = flag_table()
        self.assertEqual(len(table), len(set(table)), "a flag is listed twice")
        self.assertTrue(RENAMED_TO <= set(table))

    def test_every_flag_boot_reads_is_in_the_table_and_the_other_way(self):
        boot = read("src/runtime/lifecycle/boot.cpp")
        start = boot.index("  // Parse command line arguments.")
        main_parse = boot[start: boot.index("LogTimestepArgs();", start)]
        read_by_parse = set(re.findall(r'lstrcmpW\(flag, L"(-[a-z-]+)"\)', main_parse))
        self.assertEqual(read_by_parse, set(flag_table()), "boot.cpp's parse and kFlags disagree")

    def test_boot_reads_flags_only_through_resolve(self):
        boot = read("src/runtime/lifecycle/boot.cpp")
        self.assertEqual(re.findall(r'lstrcmpW\((?:arg|argv\[i\]), L"-', boot), [],
                         "an exact comparison of a raw token reads only one spelling (#697)")
        self.assertIn("nevr_cli::Resolve(arg)", boot)
        self.assertIn("NoteCommandLineToken(arg, named)", boot)
        note = boot[boot.index("static void NoteCommandLineToken"):boot.index("static void LogTimestepArgs")]
        self.assertIn("named.rejected", note)
        self.assertIn("more than two leading dashes", note, "three dashes are refused with a Warning")

    def test_no_alias_machinery_is_left(self):
        for rel in ("src/core/cli_flags.h", "src/runtime/lifecycle/boot.cpp"):
            text = read(rel)
            for name in ("AliasReports", "kAliasWarningFormat", "alias"):
                self.assertNotIn(name, text, f"{rel}: {name} is alias machinery (removed in #698)")


class GameParserRegistration(unittest.TestCase):
    def setUp(self):
        self.cli = read("src/runtime/lifecycle/cli.cpp")
        self.syntax = set(re.findall(r'AddArgSyntax\(pArgSyntax, "(-[a-z-]+)"', self.cli))
        self.helped = set(re.findall(r'AddArgHelpString\(pArgSyntax,\s*"(-[a-z-]+)"', self.cli))

    def test_each_removed_name_is_unknown_to_the_game(self):
        # ValidateMainArgs rejects a token with no syntax entry, so a removed name must have none.
        for name in REMOVED:
            self.assertNotIn(name, self.syntax, f"{name} is still registered: the game would accept it")
            self.assertNotIn(name, self.helped, f"{name}: --help must not list it")

    def test_each_renamed_flag_is_registered_and_documented(self):
        for flag in RENAMED_TO:
            self.assertIn(flag, self.syntax, f"{flag}: the game rejects a token with no syntax entry")
            self.assertIn(flag, self.helped, f"{flag}: the new name is documented")

    def test_help_text_holds_no_removed_name(self):
        for line in self.cli.splitlines():
            if "AddArgSyntax" in line:
                continue
            for name in REMOVED:
                self.assertIsNone(re.search(r"(?<![\w-])" + re.escape(name) + r"\b", line), line)


class NoRemovedNamesElsewhere(unittest.TestCase):
    def test_no_tracked_file_outside_the_pinning_code_carries_a_removed_name(self):
        files = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True, check=True).stdout.split("\n")
        pattern = re.compile(r"(?<![\w-])(?:" + "|".join(re.escape(name) for name in REMOVED) + r")\b")
        offenders = []
        for rel in files:
            if not rel or rel in MAY_NAME_REMOVED or rel.startswith(MAY_NAME_PREFIXES):
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
