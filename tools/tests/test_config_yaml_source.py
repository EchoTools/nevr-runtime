"""#1255: config.yaml is read from -config-file or the nevr root, and nothing of ours is written in the game folder for it.

Source-shape sensors (the behaviour is src/runtime/tests/test_config_yaml_choice.cpp, run under Wine). Code is read with its
comments removed, so the prose that explains a rule cannot satisfy its check:

  - the old `_local\\config.yaml` paths are built in one place (config_yaml_choice.h) and no file outside it builds one;
  - the file-system half never writes, renames or deletes: its one write is CopyFileW with fail-if-exists, whose destination is the
    root's path, and its other file call opens for reading;
  - the choice is made before the first read and before the game starts: boot.cpp resolves it in the preflight, ahead of the early
    plugin pass, and every rejection goes through RejectCommandLine (exit code 64, no ExitProcess, no dialog);
  - `-config-path` is registered for the game's parser and refused by the runtime; `-config` never decides where config.yaml is read.
"""

import re
import subprocess
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments

ROOT = Path(__file__).resolve().parents[2]

CHOICE = "src/runtime/lifecycle/config_yaml_choice.h"
SOURCE = "src/runtime/lifecycle/config_yaml_source.cpp"
SERVICE = "src/runtime/lifecycle/service_config.cpp"
REJECT = "src/runtime/lifecycle/cli_reject.cpp"
BOOT = "src/runtime/lifecycle/boot.cpp"
CLI = "src/runtime/lifecycle/cli.cpp"

# What changes a file: any of these on a path of ours would write into the game folder.
WRITE_APIS = re.compile(
    r"DeleteFile|MoveFile|ReplaceFile|SetFileTime|SetFileAttributes|RemoveDirectory|CreateDirectory|std::ofstream|std::fstream|"
    r"\bfopen\b|\b_wfopen\b|GENERIC_WRITE|CREATE_ALWAYS|CREATE_NEW|OPEN_ALWAYS|TRUNCATE_EXISTING|\bWriteFile\s*\(|"
    r"\bremove\s*\(|\b_wremove\b|\brename\s*\(|std::filesystem"
)


def code(path: str) -> str:
    return strip_comments((ROOT / path).read_text())


class TheLegacyPathsAreBuiltInOnePlace(unittest.TestCase):
    def test_the_local_folder_name_appears_only_in_the_choice_header(self):
        self.assertRegex(code(CHOICE), r'"_local\\\\"', "the legacy candidates are built here")
        for path in (SOURCE, SERVICE, REJECT, BOOT):
            self.assertNotIn("_local", code(path), f"{path} builds a _local path")

    def test_no_other_runtime_or_core_file_builds_a_local_config_yaml_path(self):
        files = subprocess.run(
            ["git", "ls-files", "src/runtime", "src/core", "src/modules", "src/abi", "src/extension", "src/launcher"],
            cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
        offenders = []
        for rel in files:
            if not rel.endswith((".cpp", ".h")) or rel == CHOICE or "/tests/" in rel:
                continue
            for number, line in enumerate(code(rel).splitlines(), 1):
                if re.search(r'_local[^\n]*config\.yaml|config\.yaml[^\n]*_local', line):
                    offenders.append(f"{rel}:{number}: {line.strip()[:100]}")
        self.assertEqual(offenders, [])


class NothingIsWrittenInTheGameFolder(unittest.TestCase):
    def test_the_choice_header_and_the_service_never_call_a_writing_api(self):
        for path in (CHOICE, SERVICE):
            self.assertIsNone(WRITE_APIS.search(code(path)), path)

    def test_the_file_system_half_writes_only_through_one_fail_if_exists_copy(self):
        text = code(SOURCE)
        copies = re.findall(r"CopyFileW\s*\(([^;]*)\)\s*;", text, re.S)
        self.assertEqual(len(copies), 1, "exactly one write")
        args = [a.strip() for a in copies[0].split(",")]
        self.assertEqual(len(args), 3, copies[0])
        self.assertEqual(args[2], "TRUE", "bFailIfExists: an existing file is never replaced")
        # Everything else it opens, it opens to read.
        for call in re.findall(r"CreateFileW\s*\(([^;]*)\)\s*;", text, re.S):
            self.assertIn("GENERIC_READ", call)
            self.assertIn("OPEN_EXISTING", call)
        stripped = text.replace("CopyFileW", "")
        self.assertIsNone(re.search(r"GENERIC_WRITE|CREATE_ALWAYS|CREATE_NEW|DeleteFile|MoveFile|ReplaceFile|SetFile|ofstream|"
                                    r"\bWriteFile\s*\(|std::filesystem|\bremove\s*\(", stripped))

    def test_the_copy_goes_from_the_legacy_file_to_the_roots_path_and_only_there(self):
        resolve = extract_braced_function(code(SOURCE), "Choice Resolve(")
        calls = re.findall(r"CopyIfAbsent\s*\(([^;]*)\)\s*;", resolve, re.S)
        self.assertEqual(len(calls), 1)
        self.assertRegex(calls[0], r"^\s*choice\.copy_from\s*,\s*choice\.root_path\s*,")
        choose = extract_braced_function(code(CHOICE), "inline Choice Choose(")
        self.assertRegex(choose, r"root_path\s*=\s*nevr_root::ConfigYamlPath\(")
        self.assertRegex(choose, r"copy_from\s*=\s*legacy")
        # The destination's folder is created below the root, never above the game folder.
        copy_fn = extract_braced_function(code(SOURCE), "bool CopyIfAbsent(")
        self.assertIn("EnsureDirectoryTree", copy_fn)


class TheChoiceIsMadeBeforeAnythingElse(unittest.TestCase):
    def test_boot_resolves_the_choice_in_the_preflight_before_the_early_plugin_pass(self):
        boot = code(BOOT)
        preflight = extract_braced_function(boot, "void PreflightRuntimeBootstrap()")
        self.assertIn("NevrCfgResolveConfigYaml()", preflight)
        self.assertLess(preflight.index("g_configYamlFlags"), preflight.index("NevrCfgResolveConfigYaml()"),
                        "the flags are read first")
        hook = extract_braced_function(boot, "UINT64 PreprocessCommandLineHook(PVOID pGame)")
        self.assertLess(hook.index("PreflightRuntimeBootstrap()"), hook.index("LoadPlugins(PluginPhase::Early)"),
                        "the first read of config.yaml is the early plugin pass")
        self.assertLess(hook.index("PreflightRuntimeBootstrap()"), hook.index("EchoVR::PreprocessCommandLine(pGame)"))

    def test_the_resolution_rejects_through_the_one_helper(self):
        service = code(SERVICE)
        self.assertIn("RejectCommandLine(", service)
        self.assertIn("DescribeRejection(", service)
        self.assertNotIn("ExitProcess", service)
        self.assertNotIn("MessageBox", service)
        self.assertNotIn("FatalError(", service)

    def test_the_helper_ends_the_process_with_code_64_through_force_fatal_exit_and_never_shows_a_dialog(self):
        text = code(REJECT)
        body = extract_braced_function(text, "void RejectCommandLine(")
        self.assertIn("ForceFatalExit(nevr_cli_reject::kExitCode)", body)
        self.assertIn("GetStdHandle(STD_ERROR_HANDLE)", text)
        self.assertNotIn("MessageBox", text)
        self.assertNotIn("Sleep", text)
        self.assertRegex(code("src/runtime/lifecycle/cli_reject.h"), r"kExitCode\s*=\s*64\b")
        self.assertIn("[NEVR.STOP]", text)
        self.assertIn("nlohmann", text, "the line is built with the JSON library, never by hand")

    def test_the_stop_line_is_not_built_by_hand(self):
        text = code(REJECT)
        self.assertNotRegex(text, r'"\{\\"')
        self.assertNotIn("snprintf", text)


class TheFlags(unittest.TestCase):
    def test_config_file_is_registered_with_one_value_and_a_help_line(self):
        cli = code(CLI)
        self.assertRegex(cli, r'AddArgSyntax\(pArgSyntax, "-config-file", 1, 1, FALSE\)')
        self.assertRegex(cli, r'AddArgHelpString\(pArgSyntax,\s*"-config-file"')

    def test_config_path_stays_registered_for_the_games_parser_and_has_no_help_line(self):
        cli = code(CLI)
        self.assertRegex(cli, r'AddArgSyntax\(pArgSyntax, "-config-path", 1, 1, FALSE\)')
        self.assertNotRegex(cli, r'AddArgHelpString\(pArgSyntax,\s*"-config-path"')

    def test_the_preflight_reads_both_flags_into_the_flags_global_and_config_path_is_never_the_other_flags_alias(self):
        preflight = extract_braced_function(code(BOOT), "void PreflightRuntimeBootstrap()")
        self.assertRegex(preflight, r'IsFlag\(arg,\s*L"-config-file"\)')
        self.assertRegex(preflight, r'IsFlag\(arg,\s*L"-config-path"\)')
        self.assertNotRegex(preflight, r'IsFlag\(arg,\s*L"-config"\)\s*\|\|\s*nevr_cli::IsFlag\(arg,\s*L"-config-path"\)')
        self.assertRegex(preflight, r"g_configYamlFlags\.config_path_given\s*=\s*true")
        self.assertRegex(preflight, r"g_configYamlFlags\.config_file_given\s*=\s*true")

    def test_the_game_json_flag_no_longer_decides_where_config_yaml_is_read(self):
        for path in (SERVICE, SOURCE, CHOICE):
            self.assertNotIn("g_customConfigPath", code(path), path)

    def test_the_help_text_of_config_no_longer_says_config_yaml_is_beside_it(self):
        cli = (ROOT / CLI).read_text()
        self.assertNotIn("config.yaml is looked for in the same directory", cli)
        self.assertNotIn("FindNevrConfigYamlPath", cli)


if __name__ == "__main__":
    unittest.main()
