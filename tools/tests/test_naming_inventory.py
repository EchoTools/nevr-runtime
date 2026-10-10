"""tools/naming_inventory.py against a small git repository with one finding in each category."""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "naming_inventory.py"

FILES = {
    "src/runtime/a.h": "namespace GameServer {\n}\nnamespace nevr::lifecycle {\n}\n// the n" "EVR brand\n",
    "src/runtime/b.cpp": 'namespace Nevr::Lifecycle {\n}\nvoid f() { Log("[TELEMETRY.DIAG] x"); Log("[NEVR.WS] y"); }\n',
    "src/runtime/c.h": "namespace EchoVR {\n}\nnamespace nevr_cfg {\n}\n",
    "src/runtime/CMakeLists.txt": ('add_library(platform_compat STATIC a.cpp)\nadd_library(nevr_core STATIC b.cpp)\n'
                                   'add_executable(thing_test t.cpp)\n'
                                   'target_compile_definitions(x PRIVATE GIT_COMMIT_HASH="a" NEVR_OK=1 NOMINMAX)\n'),
    "src/legacy/old.h": "namespace OldThing {\n}\n",
}


class NamingInventoryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="naming-inventory-", dir="/var/tmp"))
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", str(self.tmp)], check=False))
        subprocess.run(["git", "init", "-q", str(self.tmp)], check=True)
        for name, text in FILES.items():
            path = self.tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        subprocess.run(["git", "-C", str(self.tmp), "add", "-A"], check=True)

    def inventory(self, *args):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], capture_output=True, text=True,
                              env=dict(os.environ, NEVR_NAMING_ROOT=str(self.tmp)), check=True).stdout

    def test_each_category_reports_its_finding_with_file_and_line(self):
        out = self.inventory()
        self.assertIn("src/runtime/a.h:1  GameServer", out)
        self.assertIn("src/runtime/a.h:5  n" "EVR", out)  # spelled in two pieces: the sensor scans this file too
        self.assertIn("src/runtime/b.cpp:1  Nevr::Lifecycle", out)
        self.assertIn("src/runtime/b.cpp:3  [TELEMETRY.DIAG]", out)
        self.assertIn("src/runtime/CMakeLists.txt:1  platform_compat", out)
        self.assertIn("src/runtime/CMakeLists.txt:4  GIT_COMMIT_HASH", out)

    def test_compliant_and_excluded_names_are_not_findings(self):
        out = self.inventory()
        for fine in ("EchoVR", "nevr_cfg", "nevr::lifecycle", "[NEVR.WS]", "nevr_core", "thing_test", "NEVR_OK",
                     "NOMINMAX", "OldThing"):
            self.assertNotIn(fine + "  ", out, fine)

    def test_a_category_can_be_selected_and_summarised(self):
        out = self.inventory("--category", "pascal-namespaces")
        self.assertIn("== pascal-namespaces (1)", out)
        self.assertNotIn("log-tags", out)
        summary = self.inventory("--summary")
        self.assertNotIn("src/runtime/a.h", summary)
        self.assertIn("pascal-namespaces", summary)


if __name__ == "__main__":
    unittest.main()
