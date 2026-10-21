"""Each checkout installs with its own vcpkg root, so concurrent builds do not share a vcpkg lock."""
from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/vcpkg_root.sh"


def git(*args, cwd):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args], cwd=cwd,
                   check=True, capture_output=True, text=True)


class VcpkgRootTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="vcpkg-root-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.source = self.tmp / "source"
        self.source.mkdir()
        git("init", "-q", cwd=self.source)
        (self.source / "vcpkg").write_text("#!/bin/sh\nexit 0\n")
        (self.source / "vcpkg").chmod(0o755)
        git("add", "-A", cwd=self.source)
        git("commit", "-q", "-m", "pin", cwd=self.source)
        self.commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.source, check=True,
                                     capture_output=True, text=True).stdout.strip()

    def checkout(self, name):
        root = self.tmp / name
        (root / "tools").mkdir(parents=True)
        shutil.copy(SCRIPT, root / "tools/vcpkg_root.sh")
        (root / ".vcpkg-commit").write_text(self.commit + "\n")
        return root

    def run_script(self, checkout):
        env = dict(os.environ, NEVR_VCPKG_SOURCE=str(self.source))
        return subprocess.run([str(checkout / "tools/vcpkg_root.sh")], env=env, capture_output=True, text=True)

    def test_a_checkout_gets_its_own_root_on_the_pinned_revision(self):
        a, b = self.checkout("a"), self.checkout("b")
        ra, rb = self.run_script(a), self.run_script(b)
        self.assertEqual(ra.returncode, 0, ra.stderr)
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertEqual(ra.stdout.strip(), str(a.resolve() / "build/vcpkg-root"))
        self.assertEqual(rb.stdout.strip(), str(b.resolve() / "build/vcpkg-root"))
        self.assertNotEqual(ra.stdout, rb.stdout)
        for out in (ra.stdout, rb.stdout):
            root = pathlib.Path(out.strip())
            head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True)
            self.assertEqual(head.stdout.strip(), self.commit)
            self.assertTrue(os.access(root / "vcpkg", os.X_OK))

    def test_a_second_call_reuses_the_root(self):
        a = self.checkout("a")
        first = self.run_script(a)
        marker = pathlib.Path(first.stdout.strip()) / "buildtrees-marker"
        marker.write_text("kept")
        second = self.run_script(a)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(marker.read_text(), "kept")

    def pin_tool(self, have, want, bootstrap_writes):
        """Commit a source whose vcpkg prints `have`, whose revision names `want`, and whose
        bootstrap-vcpkg.sh replaces the executable with one that prints `bootstrap_writes`."""
        (self.source / "vcpkg").write_text(f"#!/bin/sh\necho 'vcpkg package management program version {have}-abc'\n")
        (self.source / "scripts").mkdir(exist_ok=True)
        (self.source / "scripts/vcpkg-tool-metadata.txt").write_text(f"VCPKG_TOOL_RELEASE_TAG={want}\n")
        boot = self.source / "bootstrap-vcpkg.sh"
        boot.write_text("#!/bin/sh\nd=$(dirname \"$0\")\ntouch \"$d/bootstrapped\"\n"
                        f"printf '#!/bin/sh\\necho \"vcpkg package management program version {bootstrap_writes}-def\"\\n' > \"$d/vcpkg\"\n"
                        "chmod +x \"$d/vcpkg\"\n")
        boot.chmod(0o755)
        git("add", "-A", cwd=self.source)
        git("commit", "-q", "-m", "tool", cwd=self.source)
        self.commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.source, check=True,
                                     capture_output=True, text=True).stdout.strip()

    def test_a_copied_tool_older_than_the_revision_names_is_bootstrapped(self):
        self.pin_tool(have="2025-12-16", want="2026-09-26", bootstrap_writes="2026-09-26")
        a = self.checkout("a")
        r = self.run_script(a)
        self.assertEqual(r.returncode, 0, r.stderr)
        root = pathlib.Path(r.stdout.strip())
        self.assertTrue((root / "bootstrapped").exists())
        self.assertIn("2026-09-26", subprocess.run([str(root / "vcpkg"), "version"], capture_output=True,
                                                   text=True).stdout)

    def test_a_copied_tool_on_the_named_release_is_kept(self):
        self.pin_tool(have="2026-09-26", want="2026-09-26", bootstrap_writes="2026-09-26")
        r = self.run_script(self.checkout("a"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((pathlib.Path(r.stdout.strip()) / "bootstrapped").exists())

    def test_a_bootstrap_that_does_not_reach_the_named_release_fails_loudly(self):
        self.pin_tool(have="2025-12-16", want="2026-09-26", bootstrap_writes="2026-01-01")
        r = self.run_script(self.checkout("a"))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("want 2026-09-26", r.stderr)

    def test_a_missing_source_fails_loudly(self):
        a = self.checkout("a")
        env = dict(os.environ, NEVR_VCPKG_SOURCE=str(self.tmp / "nope"))
        r = subprocess.run([str(a / "tools/vcpkg_root.sh")], env=env, capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not a vcpkg checkout", r.stderr)

    def test_the_install_recipe_uses_the_checkout_root_and_configure_does_not_reinstall(self):
        justfile = (REPO / "justfile").read_text()
        recipe = justfile[justfile.index("_vcpkg-mingw:"):]
        recipe = recipe[:recipe.index("\n\n") if "\n\n" in recipe else len(recipe)]
        self.assertIn("tools/vcpkg_root.sh", recipe)
        self.assertNotIn('cd "$HOME/.vcpkg"', recipe)
        presets = json.loads((REPO / "CMakePresets.json").read_text())
        base = next(p for p in presets["configurePresets"] if p["name"] == "mingw-base")
        self.assertEqual(base["cacheVariables"]["VCPKG_MANIFEST_INSTALL"], "OFF")
        self.assertRegex(justfile, re.compile(r"worktree-setup:\n    tools/worktree-setup\.sh\n    tools/vcpkg_root\.sh"))


if __name__ == "__main__":
    unittest.main()
