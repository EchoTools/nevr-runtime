"""Every source named in a set_source_files_properties() of src/runtime/CMakeLists.txt must exist.

CMake silently ignores properties set on a path that matches no source, so a file that moved
(ws_bridge.cpp -> compat/ws_bridge.cpp) kept its stale SKIP_PRECOMPILE_HEADERS line and compiled with the
precompiled header it was meant to skip (#189).
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
RUNTIME = REPO / "src/runtime"
CMAKE = RUNTIME / "CMakeLists.txt"


def strip_comments(text: str) -> str:
    return "\n".join(re.sub(r"(?<!\\)#.*", "", line) for line in text.splitlines())


def property_sources(text: str):
    """Yield (statement start line, path) for each file named in a set_source_files_properties call."""
    code = strip_comments(text)
    for match in re.finditer(r"set_source_files_properties\s*\((.*?)\)", code, re.S):
        body = match.group(1)
        files = re.split(r"\bPROPERTIES\b", body, maxsplit=1)[0]
        line = code[: match.start()].count("\n") + 1
        for token in files.split():
            yield line, token


class CMakeSourcePropertiesTest(unittest.TestCase):
    def test_every_path_given_to_set_source_files_properties_exists(self):
        missing = []
        seen = 0
        for line, token in property_sources(CMAKE.read_text()):
            if "$" in token or token.startswith(("\"${", "${")):
                continue  # variables are resolved by CMake; only literal paths can be checked here
            seen += 1
            path = RUNTIME / token.strip('"')
            if not path.is_file():
                missing.append(f"{CMAKE.relative_to(REPO)}:{line}: {token}")
        self.assertGreater(seen, 20, "subject vanished: found almost no literal source paths")
        self.assertEqual(missing, [], "set_source_files_properties names a path that is not a source file")

    def test_the_ws_bridge_pch_skip_names_the_real_file(self):
        names = [t for _, t in property_sources(CMAKE.read_text())]
        self.assertIn("compat/ws_bridge.cpp", names)
        self.assertNotIn("ws_bridge.cpp", names)


if __name__ == "__main__":
    unittest.main()
