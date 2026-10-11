"""Source contracts for the build-time embedded defaults (config/public-defaults.env -> generated header).

These never read a secret value: they check where secrets can and cannot go.
"""

from pathlib import Path
import re
import subprocess
import unittest


REPO = Path(__file__).resolve().parents[2]
HEADER_INCLUDE = "generated/nevr_builtin_defaults.h"


def source(relative_path: str) -> str:
    return (REPO / relative_path).read_text(encoding="utf-8")


class BuiltinDefaultsContractTest(unittest.TestCase):
    def test_dotenv_is_runtime_only_git_ignored_and_example_holds_no_values(self):
        ignore = source(".gitignore").splitlines()
        self.assertIn("/.env", ignore)
        for line in source(".env.example").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            key, _, value = stripped.partition("=")
            self.assertRegex(key, r"^NEVR_[A-Z_]+$")
            self.assertEqual(value, "", f"{key} in .env.example must be empty")

    def test_defaults_file_is_git_ignored_and_the_example_names_exactly_the_four_keys_empty(self):
        self.assertIn("/config/public-defaults.env", source(".gitignore").splitlines())
        tracked = subprocess.run(["git", "-C", str(REPO), "ls-files", "--", "config/public-defaults.env"],
                                 capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(tracked, "", "the defaults file must not be committed")
        keys = []
        for line in source("config/public-defaults.env.example").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            key, sep, value = stripped.partition("=")
            self.assertEqual(sep, "=", stripped[:20])
            self.assertEqual(value, "", f"{key} in the example must be empty")
            keys.append(key)
        self.assertEqual(sorted(keys), ["NEVR_HTTP_URI", "NEVR_PUBLIC_API_KEY",
                                        "NEVR_PUBLIC_SOCKET_KEY", "NEVR_SOCKET_URI"])

    def test_a_defaults_file_that_is_present_has_the_four_keys_filled(self):
        path = REPO / "config" / "public-defaults.env"
        if not path.exists():
            self.skipTest("no config/public-defaults.env here (CI writes it before the build)")
        values = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            key, sep, value = stripped.partition("=")
            self.assertEqual(sep, "=", stripped[:20])
            self.assertNotIn(key, values, f"{key} defined twice")
            values[key] = value
        self.assertEqual(sorted(values), ["NEVR_HTTP_URI", "NEVR_PUBLIC_API_KEY",
                                          "NEVR_PUBLIC_SOCKET_KEY", "NEVR_SOCKET_URI"])
        for key, value in values.items():
            self.assertNotEqual(value, "", key)
            self.assertNotRegex(value, r"[\s\"';\\$]", key)
        self.assertRegex(values["NEVR_SOCKET_URI"], r"^wss://[^/?#]+/nevr([?].*)?$")

    def test_ci_writes_the_defaults_file_from_actions_variables_and_never_from_secrets(self):
        action = source(".github/actions/write-public-defaults/action.yml")
        self.assertIn("config/public-defaults.env", action)
        self.assertIn("exit 1", action)  # an unset variable fails the run
        for relative in (".github/workflows/build.yml", ".github/workflows/defender-scan.yml",
                         ".github/workflows/android.yml"):
            text = source(relative)
            self.assertIn("./.github/actions/write-public-defaults", text, relative)
            for name in ("NEVR_SOCKET_URI", "NEVR_HTTP_URI", "NEVR_PUBLIC_API_KEY", "NEVR_PUBLIC_SOCKET_KEY"):
                self.assertIn("vars." + name, text, f"{relative} must read {name} from the Actions variables")
            self.assertNotRegex(text, r"secrets\.NEVR_", relative)
            # The file has to exist before the step that configures the build.
            code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
            builds = [marker for marker in ("cmake --preset", "just test-android") if marker in code]
            self.assertTrue(builds, relative)
            self.assertLess(code.index("write-public-defaults"), min(code.index(m) for m in builds), relative)

    def test_build_reads_only_the_defaults_file_never_dotenv_or_the_environment(self):
        module = source("cmake/nevr_builtin_defaults.cmake")
        code = "\n".join(line for line in module.splitlines() if not line.lstrip().startswith("#"))
        self.assertIn("config/public-defaults.env", code)
        self.assertNotRegex(code, r"\$ENV\{|(?<![\w.-])\.env\b|set\(ENV\{|getenv")
        self.assertNotIn("NEVR_REQUIRE_BUILTIN_DEFAULTS", code)
        # No other build file reads .env or the old require switch either.
        for relative in ("CMakeLists.txt", "src/quest/CMakeLists.txt", "justfile",
                         ".github/workflows/build.yml", ".github/workflows/defender-scan.yml"):
            text = source(relative)
            self.assertNotIn("NEVR_REQUIRE_BUILTIN_DEFAULTS", text, relative)

    def test_generated_header_template_has_only_placeholders(self):
        template = source("cmake/nevr_builtin_defaults.h.in")
        literals = re.findall(r'=\s*"([^"]*)";', template)
        self.assertEqual(len(literals), 4)
        for literal in literals:
            self.assertRegex(literal, r"^@NEVR_DEFAULT_[A-Z_]+@$")

    def test_only_the_pc_and_quest_config_adapters_include_the_generated_header(self):
        offenders = []
        for root in ("src", "plugins"):
            for path in (REPO / root).rglob("*"):
                if path.suffix not in {".cpp", ".h", ".hpp", ".cc"}:
                    continue
                if "legacy" in path.parts:
                    continue
                if HEADER_INCLUDE in path.read_text(encoding="utf-8", errors="replace"):
                    offenders.append(str(path.relative_to(REPO)))
        # The PC adapter and the Quest adapter are the only two readers of the embedded values.
        self.assertEqual(sorted(offenders),
                         ["src/quest/sentinel/activation.cpp", "src/runtime/lifecycle/service_config.cpp"])

    def test_quest_activation_reads_each_value_once_and_never_logs_it(self):
        text = source("src/quest/sentinel/activation.cpp")
        for value_name in ("kSocketUri", "kHttpUri", "kPublicApiKey", "kPublicSocketKey"):
            self.assertEqual(text.count(f"nevr_builtin::{value_name}"), 1, value_name)
        for call in re.findall(r"Emit\(.*?\);", text, re.S):
            self.assertNotIn("nevr_builtin::", call)
            self.assertNotIn("defaults.", call)

    def test_values_are_not_passed_on_the_compiler_command_line(self):
        module = source("cmake/nevr_builtin_defaults.cmake")
        # Comments may mention -D; code must not define the keys as compile definitions.
        code = "\n".join(line for line in module.splitlines() if not line.lstrip().startswith("#"))
        self.assertNotRegex(code, r"add_compile_definitions|target_compile_definitions|add_definitions")
        self.assertIn("FILE_PERMISSIONS OWNER_READ OWNER_WRITE", code)
        # Values are reported by length only.
        self.assertNotRegex(code, r'message\([^)]*\$\{raw\}')

    def test_runtime_logs_key_names_not_values(self):
        text = source("src/runtime/lifecycle/service_config.cpp")
        start = text.index("const nevr_cfg::FlatDefaults& BuiltinDefaults()")
        end = text.index("// One lookup for every flat key", start)
        body = text[start:end]
        for value_name in ("kSocketUri", "kHttpUri", "kPublicApiKey", "kPublicSocketKey"):
            self.assertEqual(body.count(f"nevr_builtin::{value_name}"), 1, value_name)
        # The only Log arguments are the joined key-name lists.
        for call in re.findall(r"Log\(.*?\);", body, re.S):
            self.assertNotIn("nevr_builtin::", call)
            self.assertNotIn("it->second", call)

    def test_defaults_are_client_only(self):
        text = source("src/runtime/lifecycle/service_config.cpp")
        # IsServerMode() is the one server-mode test; outside test builds it is g_isServer.
        self.assertRegex(text, r"bool IsServerMode\(\) \{(?:\s*#[^\n]*\n[^\n]*\n[^\n]*)?\s*return g_isServer != FALSE;")
        # The gate itself is the pure SelectBuiltinDefaults (tested in test_service_map.cpp); production
        # passes the one server-mode test into it.
        self.assertRegex(text, r"SelectBuiltinDefaults\(\s*IsServerMode\(\),")
        self.assertRegex(text, r"if \(IsServerMode\(\)\) \{\s*Log\(EchoVR::LogLevel::Info,\s*\"\[NEVR\.CONFIG\] built-in defaults are not applied in server mode")


if __name__ == "__main__":
    unittest.main()
