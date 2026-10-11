"""check_embedded_defaults.py: the proof that a build embeds exactly config/public-defaults.env."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "check_embedded_defaults.py"

VALUES = {
    "NEVR_SOCKET_URI": "wss://host.example/nevr",
    "NEVR_HTTP_URI": "https://host.example",
    "NEVR_PUBLIC_API_KEY": "public-api-key-value",
    "NEVR_PUBLIC_SOCKET_KEY": "public-socket-key-value",
}
NAMES = {"NEVR_SOCKET_URI": "kSocketUri", "NEVR_HTTP_URI": "kHttpUri",
         "NEVR_PUBLIC_API_KEY": "kPublicApiKey", "NEVR_PUBLIC_SOCKET_KEY": "kPublicSocketKey"}


def header(values):
    body = "".join(f'inline constexpr const char* {NAMES[k]} = "{v}";\n' for k, v in values.items())
    return "#pragma once\nnamespace nevr_builtin {\n" + body + "}\n"


class CheckEmbeddedDefaultsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="check-defaults-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        self.defaults = self.tmp / "public-defaults.env"
        self.defaults.write_text("# c\n" + "".join(f"{k}={v}\n" for k, v in VALUES.items()))
        self.header = self.tmp / "nevr_builtin_defaults.h"
        self.header.write_text(header(VALUES))
        self.binary = self.tmp / "client.bin"
        self.binary.write_bytes(b"\0".join(v.encode() for v in VALUES.values()))

    def run_tool(self, *extra):
        return subprocess.run([sys.executable, "-I", str(TOOL), "--defaults", str(self.defaults),
                               "--header", str(self.header), *extra], capture_output=True, text=True)

    def test_matching_header_and_binary_pass(self):
        result = self.run_tool("--binary", str(self.binary))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_header_value_that_differs_from_the_file_fails_naming_the_key_not_the_value(self):
        changed = dict(VALUES, NEVR_HTTP_URI="https://elsewhere.example")
        self.header.write_text(header(changed))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("NEVR_HTTP_URI", result.stderr)
        for value in list(VALUES.values()) + ["https://elsewhere.example"]:
            self.assertNotIn(value, result.stderr + result.stdout)

    def test_a_binary_missing_a_value_fails(self):
        self.binary.write_bytes(b"\0".join(v.encode() for k, v in VALUES.items() if k != "NEVR_PUBLIC_API_KEY"))
        result = self.run_tool("--binary", str(self.binary))
        self.assertEqual(result.returncode, 1)
        self.assertIn("NEVR_PUBLIC_API_KEY: not embedded in client.bin", result.stderr)

    def test_an_empty_or_missing_key_in_the_file_fails(self):
        self.defaults.write_text("".join(f"{k}={v}\n" for k, v in VALUES.items() if k != "NEVR_SOCKET_URI"))
        self.assertEqual(self.run_tool().returncode, 1)
        self.defaults.write_text("NEVR_SOCKET_URI=\n")
        self.assertEqual(self.run_tool().returncode, 1)

    def test_a_defaults_file_that_is_present_passes_its_own_parser(self):
        path = REPO / "config" / "public-defaults.env"
        if not path.exists():
            self.skipTest("no config/public-defaults.env here (CI writes it before the build)")
        sys.path.insert(0, str(REPO / "tools"))
        try:
            import check_embedded_defaults as tool
        finally:
            sys.path.pop(0)
        values = tool.read_defaults(path)
        self.assertEqual(sorted(values), sorted(VALUES))


if __name__ == "__main__":
    unittest.main()
