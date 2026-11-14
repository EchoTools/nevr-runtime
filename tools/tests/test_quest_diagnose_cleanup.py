# ADR 0044 recorded reason: tests the Quest diagnose cleanup that tools/quest-diagnose.sh runs, which stays Python-tested until #2132 ports that tool.
"""tools/package-release/quest-diagnose.ps1: the PC-side cleanup removes only what the script wrote (ORDER.md "The manifest rule").

The script's temporary folder is cleaned by Remove-RecordedWork, which takes the files and folders the script recorded as it
wrote them. These tests take that one function out of the script (its syntax tree, so the function is the script's own text),
run it under pwsh on a scratch tree, and check what is left. They are skipped when pwsh is not installed; the text checks run
everywhere.
"""
from __future__ import annotations

import json
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "package-release" / "quest-diagnose.ps1"

HARNESS = r'''
param([string]$Script, [string]$SpecPath)
$ErrorActionPreference = 'Stop'
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref]$null, [ref]$errors)
if ($errors.Count -gt 0) { throw "the script does not parse: $($errors[0].Message)" }
$definition = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Remove-RecordedWork' }, $true) | Select-Object -First 1
if (-not $definition) { throw 'Remove-RecordedWork is not defined in the script' }
. ([scriptblock]::Create($definition.Extent.Text))
$specText = Get-Content -LiteralPath $SpecPath -Raw
$specObj = $specText | ConvertFrom-Json
$left = Remove-RecordedWork -Root $specObj.root -Files @($specObj.files) -Folders @($specObj.folders)
Write-Output ("left=" + $left)
'''


def remove_item_lines(text: str):
    """Every line that calls Remove-Item, with its line number."""
    return [(number, line) for number, line in enumerate(text.splitlines(), 1)
            if re.search(r"\bRemove-Item\b", line) and not line.lstrip().startswith("#")]


class DiagnoseCleanupText(unittest.TestCase):
    def setUp(self):
        self.text = SCRIPT.read_text(encoding="utf-8")

    def test_no_remove_item_recurses(self):
        for number, line in remove_item_lines(self.text):
            with self.subTest(line=number):
                self.assertNotRegex(line, r"-Recurse\b", f"quest-diagnose.ps1:{number} removes a tree: {line.strip()}")

    def test_the_removal_is_one_function_that_takes_what_was_recorded(self):
        self.assertRegex(self.text, r"function Remove-RecordedWork\(\[string\]\$Root, \[string\[\]\]\$Files, \[string\[\]\]\$Folders\)")
        self.assertRegex(self.text, r"\$script:WorkFiles\s*=\s*New-Object")
        self.assertRegex(self.text, r"\$script:WorkFolders\s*=\s*New-Object")
        self.assertRegex(self.text, r"Remove-RecordedWork \$script:Work @\(\$script:WorkFiles\) @\(\$script:WorkFolders\)")


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class DiagnoseCleanupRuns(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = pathlib.Path(self.tmp.name)
        self.harness = self.base / "harness.ps1"
        self.harness.write_text(HARNESS, encoding="utf-8")
        self.work = self.base / "nevr-diagnose-test"
        self.work.mkdir()

    def make(self, relative, text="x"):
        path = self.work / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def run_removal(self, files, folders):
        spec = self.base / "spec.json"
        spec.write_text(json.dumps({"root": str(self.work), "files": [str(f) for f in files], "folders": [str(f) for f in folders]}), encoding="utf-8")
        done = subprocess.run([shutil.which("pwsh"), "-NoProfile", "-File", str(self.harness), "-Script", str(SCRIPT), "-SpecPath", str(spec)],
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return done.stdout

    def test_only_the_recorded_files_and_empty_folders_are_removed(self):
        wrote = self.make("sentinel/nevr-a.jsonl")
        self.make("sentinel/nevr-b.jsonl")  # written by the script but not recorded: stays
        self.make("device.txt")  # recorded
        self.make("stranger/keep.txt")  # never recorded, in a folder that is not recorded: stays
        empty = self.work / "listings"
        empty.mkdir()
        kept_folder = self.work / "sentinel"
        out = self.run_removal([wrote, self.work / "device.txt"], [empty, kept_folder, self.work])
        self.assertIn("left=2", out)  # sentinel still holds nevr-b.jsonl and the work folder holds stranger/: both stay
        self.assertFalse(wrote.exists())
        self.assertFalse((self.work / "device.txt").exists())
        self.assertFalse(empty.exists())
        self.assertTrue((self.work / "sentinel" / "nevr-b.jsonl").is_file())
        self.assertTrue((self.work / "stranger" / "keep.txt").is_file())
        self.assertTrue(self.work.is_dir())

    def test_a_folder_that_is_not_empty_is_never_removed_even_when_recorded(self):
        self.make("trees/inner/unlisted.bin")
        out = self.run_removal([], [self.work / "trees" / "inner", self.work / "trees", self.work])
        self.assertIn("left=3", out)
        self.assertTrue((self.work / "trees" / "inner" / "unlisted.bin").is_file())

    def test_a_recorded_path_outside_the_work_folder_is_never_touched(self):
        outside = self.base / "outside.txt"  # a sibling of the work folder, not under it
        outside.write_text("keep me", encoding="utf-8")
        escape = self.work / ".." / "outside.txt"  # the traversal form, resolved to the same file
        out = self.run_removal([escape, outside], [self.base])
        self.assertIn("left=3", out)  # both file entries and the folder above the work folder are left
        self.assertEqual(outside.read_text(encoding="utf-8"), "keep me")
        self.assertTrue(self.base.is_dir())

    def test_a_missing_recorded_entry_is_not_an_error(self):
        out = self.run_removal([self.work / "never-written.txt"], [])
        self.assertIn("left=0", out)


if __name__ == "__main__":
    unittest.main()
