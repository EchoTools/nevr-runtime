import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function


ROOT = Path(__file__).resolve().parents[2]


class CrashHandlerPluginSource(unittest.TestCase):
    def test_init_treats_mh_already_initialized_as_benign(self):
        """#26: MinHook's global init is process-wide, so a plugin loaded after another
        one sees MH_ERROR_ALREADY_INITIALIZED. Every sibling plugin accepts it; treating
        it as fatal aborts the crash handler's own init. The plugin is not built by
        CMake today, so the source is checked directly."""
        source = (ROOT / "plugins/crash-handler/src/plugin.cpp").read_text()
        body = extract_braced_function(source, "NEVR_PLUGIN_API int NvrPluginInit(")
        guard = re.search(r"if\s*\(\s*mhStatus\s*!=\s*MH_OK\s*&&\s*mhStatus\s*!=\s*MH_ERROR_ALREADY_INITIALIZED\s*\)", body)
        self.assertIsNotNone(guard, "MH_Initialize's status check must accept MH_ERROR_ALREADY_INITIALIZED")


if __name__ == "__main__":
    unittest.main()
