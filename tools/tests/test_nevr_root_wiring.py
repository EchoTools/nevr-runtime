"""#857: every place nEVR writes or reads resolves through core/nevr_root.h, and the one-time move runs first.
KNOWN-WRONG #1232: the start-up log is still written beside echovr.exe. KNOWN-WRONG #1255: config.yaml is not read from
the root.

Source-shape sensors (the behaviour is src/runtime/tests/test_nevr_root.cpp, run under Wine):

  - the run log, the crash record, the asset cache and the cosmetic packages each name nevr_root, and none of those files names
    the original game's %LOCALAPPDATA%\\EchoVR folder; the sign-in cache is the root's .credentials.json (the move takes it from an
    install's _local folders through the protected write, nothing else);
  - initialize.cpp runs the move after the boot log opens and before the main log, the config or any cache is opened;
  - the host's own folder names are tied to the same names at compile time;
  - the test target exists and both justfile lists run it.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


class EveryWriterUsesTheRoot(unittest.TestCase):
    def test_each_site_resolves_through_nevr_root(self):
        sites = (
            ("src/runtime/log/builtin_filter.cpp", "static std::string GetDefaultLogDir(", "nevr_root::LogsDir("),
            ("src/runtime/lifecycle/crash_recovery.cpp", "static void PrepareCrashRecordPath(", "nevr_root::LogsDir("),
            ("src/runtime/patch/asset_cache.cpp", "std::wstring DefaultDir(", "nevr_root::CacheDir("),
            ("src/runtime/patch/asset_cdn.cpp", "std::string nevr_asset_cdn::GetCacheDir(", "nevr_root::CosmeticsDir("),
        )
        for path, signature, call in sites:
            with self.subTest(path=path):
                self.assertIn(call, extract_braced_function(read(path), signature))

    def test_no_writer_names_the_original_games_folder(self):
        for path in (
            "src/runtime/log/builtin_filter.cpp",
            "src/runtime/lifecycle/crash_recovery.cpp",
            "src/runtime/patch/asset_cache.cpp",
            "src/runtime/patch/asset_cdn.cpp",
        ):
            code = strip_comments(read(path))
            with self.subTest(path=path):
                self.assertNotIn("EchoVR\\\\logs", code)
                self.assertNotIn("\\\\EchoVR\\\\", code)
                self.assertNotIn("cosmetics\\\\v1", code, "the cosmetics folder is named once, in nevr_root.h")


class TheMoveRunsFirst(unittest.TestCase):
    def test_initialize_runs_the_move_between_the_boot_log_and_the_main_log(self):
        body = extract_braced_function(read("src/runtime/lifecycle/initialize.cpp"), "static VOID InitializeAfterGameImageGuard()")
        boot = body.index("nevr_boot_log_tee::Init()")
        move = body.index("nevr_root_move::RunAndLog()")
        log_filter = body.index("nevr_builtin_log_filter::Init(")
        self.assertLess(boot, move, "Log() reports through the boot tee only once it is open")
        self.assertLess(move, log_filter, "the main log must open in the root after the old logs have been moved")

    def test_the_boot_line_names_the_new_log_folder(self):
        code = read("src/runtime/lifecycle/initialize.cpp")
        self.assertIn("%%LOCALAPPDATA%%\\\\nevr\\\\echovr\\\\logs\\\\nevr-<timestamp>.jsonl", code)
        self.assertNotIn("%%LOCALAPPDATA%%\\\\EchoVR", code)

    def test_the_move_takes_only_the_sign_in_cache_from_the_installs_local_folders(self):
        code = strip_comments(read("src/runtime/lifecycle/nevr_root_move.cpp"))
        # KNOWN-WRONG #1255: config.yaml stays in _local. No assertion here pins that it is never moved.
        self.assertEqual(len(re.findall(r"Names::kCredentials", code)), 1, "the one file taken from _local")
        self.assertIn("nevr_credential_write::WriteProtectedTemp", code, "the cache is moved through the protected write")

    def test_the_cache_is_selected_in_the_root_first_with_the_older_search_as_the_fallback(self):
        code = strip_comments(read("src/core/auth_token.h"))
        body = extract_braced_function(
            code, "inline CredentialCacheLocation SelectCredentialCacheLocation(const std::string& exeDir, const std::string& root)")
        self.assertIn("nevr_root::CredentialsPath(root)", body)
        self.assertLess(body.index("nevr_root::CredentialsPath(root)"), body.index("kLocalSuffixes"))
        self.assertIn("SelectLegacyCredentialCacheLocation(exeDir)", body)

    def test_the_move_deletes_only_what_it_moved(self):
        code = strip_comments(read("src/runtime/lifecycle/nevr_root_move.cpp"))
        deletes = re.findall(r"DeleteFileW\(([a-z_]+)\.c_str\(\)\)", code)
        self.assertEqual(sorted(deletes), ["from", "wide_temp"], "the moved cache's source, and the move's own temp file when it failed its check")

    def test_the_cache_is_installed_create_only_through_a_temp_file_of_its_own(self):
        code = strip_comments(read("src/runtime/lifecycle/nevr_root_move.cpp"))
        self.assertIn("nevr_credential_write::InstallCacheIfAbsent", code, "a cache already in the root is never replaced")
        self.assertNotIn("ReplaceCache", code, "the replacing rename is the sign-in cache's own save, not the one-time move")
        self.assertIn("nevr_credential_write::UniqueTempPathFor", code, "the cache's save shares TempPathFor; the move has its own name")
        self.assertIsNone(re.search(r"(?<!Unique)TempPathFor\(", code), "no use of the shared temp name")
        self.assertNotIn("DeleteFileW(to", code, "a cache in the root is never deleted by the move")

    def test_the_move_never_overwrites_and_deletes_nothing(self):
        code = strip_comments(read("src/runtime/lifecycle/nevr_root_move.cpp"))
        self.assertIn("MOVEFILE_COPY_ALLOWED", code)
        self.assertNotIn("MOVEFILE_REPLACE_EXISTING", code)
        self.assertNotIn("DeleteFileA", code)


class TheHostAndTheRuntimeNameTheSameFolders(unittest.TestCase):
    def test_host_paths_are_tied_to_nevr_root_at_compile_time(self):
        code = strip_comments(read("src/runtime/host/host_paths.h"))
        self.assertIn('#include "core/nevr_root.h"', read("src/runtime/host/host_paths.h"))
        self.assertIn("static_assert(IsRootChild(kLogsSubdirectory, nevr_root::Names<wchar_t>::kLogs)", code)
        self.assertIn("static_assert(IsRootChild(kCrashesSubdirectory, nevr_root::Names<wchar_t>::kCrashes)", code)


class TheTestRuns(unittest.TestCase):
    def test_the_target_is_built_and_both_justfile_lists_run_it(self):
        self.assertIn("add_executable(test_nevr_root", read("src/runtime/CMakeLists.txt"))
        justfile = read("justfile")
        self.assertIn("--target test_nevr_root", justfile)
        loops = [m for m in re.findall(r"for test_name in ([^;]*); do", justfile) if "test_asset_cache" in m]
        self.assertEqual(len(loops), 1)
        self.assertIn("test_nevr_root", loops[0].split())


if __name__ == "__main__":
    unittest.main()
