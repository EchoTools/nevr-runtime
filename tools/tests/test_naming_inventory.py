"""tools/naming_inventory.py against a small git repository with one finding in each category."""
from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "naming_inventory.py"

FILES = {
    "src/runtime/a.h": "namespace GameServer {\n}\nnamespace nevr::lifecycle {\n}\n// the n" "EVR brand\n",
    "src/runtime/b.cpp": 'namespace Nevr::' 'Lifecycle {\n}\nvoid f() { Log("[TELEMETRY.DIAG] x"); Log("[NEVR.WS] y"); }\n',
    "src/runtime/c.h": "namespace EchoVR {\n}\nnamespace nevr_cfg {\n}\n",
    "src/runtime/CMakeLists.txt": ('add_library(platform_compat STATIC a.cpp)\nadd_library(nevr_core STATIC b.cpp)\n'
                                   'add_executable(thing_test t.cpp)\n'
                                   'target_compile_definitions(x PRIVATE GIT_COMMIT_HASH="a" NEVR_OK=1 NOMINMAX)\n'),
    "extern/old.h": "namespace OldThing {\n}\n",
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
        self.assertIn("src/runtime/b.cpp:1  Nevr::" "Lifecycle", out)  # two pieces: this is Python text, and the lifecycle category reads code files only
        self.assertIn("src/runtime/b.cpp:3  [TELEMETRY.DIAG]", out)
        self.assertIn("src/runtime/CMakeLists.txt:1  platform_compat", out)
        self.assertIn("src/runtime/CMakeLists.txt:4  GIT_COMMIT_HASH", out)

    def test_compliant_and_excluded_names_are_not_findings(self):
        out = self.inventory()
        for fine in ("EchoVR", "nevr_cfg", "nevr::lifecycle", "[NEVR.WS]", "nevr_core", "thing_test", "NEVR_OK",
                     "NOMINMAX", "OldThing"):
            self.assertNotIn(fine + "  ", out, fine)

    def test_a_reconstruction_namespace_is_a_finding_only_when_it_breaks_naming_md(self):
        # docs/standards/naming.md: the game's namespaces in src/reconstruction keep the game's spelling, and a namespace
        # the reconstruction wrote takes the rule on its last component, after the game's prefix. The literals are split:
        # this file is scanned too, and a whole `namespace X {` line here would be a finding of the real tree.
        path = "src/reconstruction/r15/a.cpp"
        lines = [
            "namespace NRadEngine::" "nevr_reconstruction {",                 # 1 allowed
            "namespace NRadEngine::NRadGame::" "nevr_reconstruction {",       # 2 allowed
            "namespace NRadEngine::" "NRadGame {",                            # 3 allowed: the game's spelling
            "namespace NRadEngine::CMemory {",                                # 4 allowed: the game's spelling
            "namespace NWriteLog {",                                          # 5 allowed: the game's spelling
            "namespace NRadEngine::" "reconstruction {",                      # 6 finding: last component not nevr
            "namespace xtool::r15::" "reconstruction {",                      # 7 finding: lower-case prefix
            "namespace NRadEngine::" "nevr_reconstruction::" "extra {",       # 8 finding: nevr is not the last component
            "namespace xtool::" "NRadGame {",                                 # 9 finding: lower-case prefix, game spelling after it
            "namespace xtool::" "nevr_reconstruction {",                      # 10 finding: lower-case prefix
            "namespace NRadEngine::" "nevr_Reconstruction {",                 # 11 finding: nevr_<area> is lower-case
            "namespace " "nevrfoo {",                                         # 12 finding: nevr needs an underscore for an area
            "namespace NRadEngine::" "nevr_ {",                               # 13 finding: an empty area
            "namespace NRadEngine::" "NevrReconstruction {",                  # 14 finding: project spelling, not the game's
            "namespace " "nevr_reconstruction {",                             # 15 allowed: bare nevr_<area>
            "namespace " "nevr {",                                            # 16 allowed: bare nevr
            "namespace NevrX::" "NRadGame {",                                 # 17 finding: project-spelled prefix, game spelling after it
            "namespace NevrX::" "nevr_reconstruction {",                      # 18 finding: project-spelled prefix
        ]
        target = self.tmp / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("\n".join(lines) + "\n", encoding="utf-8")
        out = self.inventory("--category", "pascal-namespaces")
        found = {int(line.split(":")[1].split()[0]) for line in out.splitlines() if line.startswith(path + ":")}
        self.assertEqual(found, {6, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18}, out)

    def test_a_namespace_outside_the_reconstruction_keeps_the_general_rule(self):
        # The reconstruction's game spelling is not a licence anywhere else: a PascalCase namespace in the tree is a finding
        # unless it is EchoVR or NRadEngine, and the four non-models naming.md names stay findings (see the next test).
        target = self.tmp / "src/runtime/d.h"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("namespace NRadEngine::" "nevr_reconstruction {\n}\nnamespace NRadEngine::CMemory {\n}\n",
                          encoding="utf-8")
        out = self.inventory("--category", "pascal-namespaces")
        self.assertIn("src/runtime/d.h:1  NRadEngine::nevr_reconstruction", out)
        self.assertIn("src/runtime/d.h:3  NRadEngine::CMemory", out)

    def test_a_category_can_be_selected_and_summarised(self):
        out = self.inventory("--category", "pascal-namespaces")
        self.assertIn("== pascal-namespaces (1)", out)
        self.assertNotIn("log-tags", out)
        summary = self.inventory("--summary")
        self.assertNotIn("src/runtime/a.h", summary)
        self.assertIn("pascal-namespaces", summary)


class NamedPathsTest(unittest.TestCase):
    # A path the inventory's text names (its docstring, SELF) must be a file of the tree, so a reference to a file that
    # is not there fails here rather than in a reader's head.
    PATH = re.compile(r"\b(?:docs|tools|src|sdk|installer)/[A-Za-z0-9_./-]+\.(?:md|py|json|h|cpp|txt|yaml|cmake)\b")

    def test_every_path_the_inventory_names_is_in_the_tree(self):
        tree = set(subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                                  check=True, capture_output=True, text=True).stdout.split("\0"))
        named = sorted(set(self.PATH.findall(SCRIPT.read_text(encoding="utf-8"))))
        self.assertTrue(named, "the check found no path in the inventory to read")
        self.assertEqual([p for p in named if p not in tree], [],
                         "tools/naming_inventory.py names a path that is not in the tree")


if __name__ == "__main__":
    unittest.main()
