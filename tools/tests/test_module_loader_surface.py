import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ModuleLoaderSurface(unittest.TestCase):
    def test_no_dynamic_module_loader(self):
        """#32: modules are statically linked and registered with RegisterStaticModule.
        LoadModule (the modules/*.dll loader) had no caller and must not come back
        unreferenced."""
        for rel in ("src/runtime/ext/module_loader.h", "src/runtime/ext/module_loader.cpp"):
            text = (ROOT / rel).read_text()
            self.assertIsNone(re.search(r"\bLoadModule\s*\(", text), f"{rel} defines or declares LoadModule")
        self.assertIn("RegisterStaticModule", (ROOT / "src/runtime/ext/module_loader.h").read_text())


if __name__ == "__main__":
    unittest.main()
