"""The runtime DLL has one file name, `nevr-runtime.dll` (docs/standards/naming.md, ADR 0012).

The name `nevr.dll` is not spelled anywhere in the tree except in the dated audits and in this test, and no build
target is given the output name `nevr`, which would produce that file. The allowed set is written out below on
purpose: a new entry is a visible edit here."""
from __future__ import annotations

import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
# The old file name, in any case (Windows file names are case-insensitive), as a whole name: not part of a
# longer one such as `pnsnevr.dll`, and not the log tag `[NEVR.DLLHOOK]`.
OLD_NAME = re.compile(r"(?<![A-Za-z0-9_-])nevr\.dll(?![A-Za-z0-9_])", re.IGNORECASE)
# A target given the output name `nevr` builds `nevr.dll`, whatever its sources say.
OLD_OUTPUT_NAME = re.compile(r"OUTPUT_NAME\s+[\"']nevr[\"']")
# Where the old name may appear: this test, and the audits, which are the dated record of the tree as it was
# (AGENTS.md "Documentation, plans and findings").
ALLOWED_FILES = frozenset({"tools/tests/test_runtime_dll_name.py"})
ALLOWED_PREFIXES = ("docs/audits/",)


def project_files():
    """Tracked and untracked-but-not-ignored text files, so a new file is checked before it is added."""
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         check=True, capture_output=True, text=True).stdout
    for path in (p for p in out.split("\0") if p):
        try:
            yield path, (REPO / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue


def checked_files():
    return [(path, text) for path, text in project_files()
            if path not in ALLOWED_FILES and not path.startswith(ALLOWED_PREFIXES)]


class RuntimeDllNameTest(unittest.TestCase):
    def test_the_scan_sees_the_tree(self):
        files = dict(checked_files())
        self.assertGreater(len(files), 300, "the sensor is reading almost nothing")
        self.assertIn("src/runtime/host/runtime_load.cpp", files)
        self.assertIn("tools/verify_pe_contract.py", files)

    def test_the_old_name_is_spelled_nowhere(self):
        found = {}
        for path, text in checked_files():
            lines = [n for n, line in enumerate(text.splitlines(), 1) if OLD_NAME.search(line)]
            if lines:
                found[path] = lines
        self.assertEqual(found, {}, "the runtime DLL is nevr-runtime.dll (docs/standards/naming.md)")

    def test_no_target_is_built_as_the_old_name(self):
        found = {}
        for path, text in checked_files():
            lines = [n for n, line in enumerate(text.splitlines(), 1) if OLD_OUTPUT_NAME.search(line)]
            if lines:
                found[path] = lines
        self.assertEqual(found, {}, 'OUTPUT_NAME "nevr" builds nevr.dll; the runtime DLL is nevr-runtime.dll')

    def test_the_loader_and_the_contract_name_the_new_file(self):
        loader = (REPO / "src/runtime/host/runtime_load.cpp").read_text(encoding="utf-8")
        contract = (REPO / "tools/verify_pe_contract.py").read_text(encoding="utf-8")
        self.assertTrue('kRuntimeFileName[] = L"nevr-runtime.dll"' in loader,
                        "src/runtime/host/runtime_load.cpp: kRuntimeFileName is not L\"nevr-runtime.dll\"")
        self.assertTrue('RUNTIME_DLL = "nevr-runtime.dll"' in contract,
                        'tools/verify_pe_contract.py: RUNTIME_DLL is not "nevr-runtime.dll"')

    def test_the_naming_standard_names_the_runtime_dll(self):
        naming = (REPO / "docs/standards/naming.md").read_text(encoding="utf-8")
        rows = [line for line in naming.splitlines() if line.startswith("| Runtime DLL file |")]
        self.assertEqual(len(rows), 1, "docs/standards/naming.md has no single 'Runtime DLL file' row")
        self.assertTrue("`nevr-runtime.dll`" in rows[0], "the 'Runtime DLL file' row does not name nevr-runtime.dll")

    def test_the_old_name_pattern_matches_what_it_must_and_nothing_else(self):
        for text in ("nevr.dll", "L\"nevr.dll\"", "NEVR.DLL", "C:\\g\\nevr.dll", "built as nevr.dll."):
            self.assertTrue(OLD_NAME.search(text), text)
        for text in ("nevr-runtime.dll", "pnsnevr.dll", "[NEVR.DLLHOOK] x", "nevr.dllx", "nevr_dll", "nevr.example"):
            self.assertFalse(OLD_NAME.search(text), text)
        self.assertTrue(OLD_OUTPUT_NAME.search('PREFIX "" OUTPUT_NAME "nevr")'))
        self.assertFalse(OLD_OUTPUT_NAME.search('OUTPUT_NAME "nevr-runtime"'))
        self.assertFalse(OLD_OUTPUT_NAME.search('OUTPUT_NAME "nevr_example"'))


if __name__ == "__main__":
    unittest.main()
