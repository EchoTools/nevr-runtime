"""verify_scenario_control_absent.py fails on a DLL that carries the endpoint, or no DLL."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import verify_scenario_control_absent as checker  # noqa: E402


class ScenarioControlAbsentTest(unittest.TestCase):
    def test_release_shaped_dll_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            dll = pathlib.Path(tmp) / "BugSplat64.dll"
            dll.write_bytes(b"MZ\x00\x00[NEVR.SOCIAL] game->server\x00")
            ok, message = checker.check(dll)
            self.assertTrue(ok, message)

    def test_dll_with_the_endpoint_fails_and_says_why(self):
        with tempfile.TemporaryDirectory() as tmp:
            dll = pathlib.Path(tmp) / "BugSplat64.dll"
            dll.write_bytes(b"MZ\x00\x00[NEVR.SCENARIO] control listening on 127.0.0.1:%u\x00")
            ok, message = checker.check(dll)
            self.assertFalse(ok)
            self.assertIn("carries the scenario control endpoint", message)

    def test_missing_dll_fails(self):
        ok, message = checker.check(pathlib.Path("/nonexistent/BugSplat64.dll"))
        self.assertFalse(ok)
        self.assertIn("does not exist", message)


if __name__ == "__main__":
    unittest.main()
