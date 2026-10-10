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

    def test_module_record_has_no_dll_state(self):
        """#246: every module is static, so the record carries no HMODULE, init pointer or path and
        UnloadModules has no FreeLibrary to call."""
        text = (ROOT / "src/runtime/ext/module_loader.cpp").read_text()
        for dead in (r"\bhModule\b", r"\bFreeLibrary\b", r"\.init\b", r"std::string\s+path\b"):
            self.assertIsNone(re.search(dead, text), f"module_loader.cpp still has {dead}")

    def test_justfile_ws_bridge_sensor_matches_static_registration(self):
        """#246: the N92B sensor grepped for a LoadModule( call that no longer exists, so it could
        not fire. Its pattern must match the ways boot.cpp could load ws_bridge as a module again."""
        line = next(l for l in (ROOT / "justfile").read_text().splitlines() if l.strip().startswith("N92B_RC=0;"))
        match = re.search(r"grep -qE '([^']+)'", line)
        self.assertIsNotNone(match, "N92B sensor must use grep -qE")
        pattern = re.compile(match.group(1).replace(r"\s", r"\s"))
        for hit in ('RegisterStaticModule("ws_bridge", 1, nullptr, nullptr, nullptr);',
                    'LoadLibraryA("ws_bridge.dll");', 'LoadModule("ws_bridge", ctx)'):
            self.assertRegex(hit, pattern)
        self.assertNotRegex("InstallWebSocketBridge();", pattern)


if __name__ == "__main__":
    unittest.main()
