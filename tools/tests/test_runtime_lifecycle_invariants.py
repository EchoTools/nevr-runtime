import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def extract_braced_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function body: {signature}")


class RuntimeLifecycleInvariantTest(unittest.TestCase):
    def test_dllmain_does_not_run_plugin_shutdown_or_unload(self):
        source = (ROOT / "src/runtime/lifecycle/dllmain.cpp").read_text()
        body = extract_braced_function(source, "BOOL APIENTRY DllMain(")

        self.assertNotRegex(body, r"\bUnloadPlugins\s*\(")
        self.assertNotIn("NvrPluginShutdown", body)


if __name__ == "__main__":
    unittest.main()
