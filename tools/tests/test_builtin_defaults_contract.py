"""Source contracts for the build-time embedded defaults (env/.env -> generated header).

These never read a secret value: they check where secrets can and cannot go.
"""

from pathlib import Path
import re
import unittest


REPO = Path(__file__).resolve().parents[2]
HEADER_INCLUDE = "generated/nevr_builtin_defaults.h"


def source(relative_path: str) -> str:
    return (REPO / relative_path).read_text(encoding="utf-8")


class BuiltinDefaultsContractTest(unittest.TestCase):
    def test_dotenv_is_git_ignored_and_example_holds_no_values(self):
        ignore = source(".gitignore").splitlines()
        self.assertIn("/.env", ignore)
        for line in source(".env.example").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            key, _, value = stripped.partition("=")
            self.assertRegex(key, r"^NEVR_[A-Z_]+$")
            self.assertEqual(value, "", f"{key} in .env.example must be empty")

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
