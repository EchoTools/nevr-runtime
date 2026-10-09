"""CI builds the vcpkg revision recorded in .vcpkg-commit, read from that one file."""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github/workflows"


class VcpkgPinTest(unittest.TestCase):
    def test_the_pin_is_one_full_commit_sha(self):
        self.assertRegex((REPO / ".vcpkg-commit").read_text(), r"\A[0-9a-f]{40}\n\Z")

    def test_every_workflow_that_clones_vcpkg_checks_out_the_pinned_revision_from_the_file(self):
        cloning = [p for p in sorted(WORKFLOWS.glob("*.yml")) if "Microsoft/vcpkg.git" in p.read_text()]
        self.assertGreaterEqual(len(cloning), 3, "expected build, vcpkg-cache and defender-scan to clone vcpkg")
        for workflow in cloning:
            text = workflow.read_text()
            self.assertRegex(text, r'checkout --detach "\$\(cat "\$GITHUB_WORKSPACE/\.vcpkg-commit"\)"',
                             f"{workflow.name} does not check out the revision in .vcpkg-commit")
            self.assertIsNone(re.search(r"\b[0-9a-f]{40}\b", text), f"{workflow.name} hard-codes a sha")

    def test_the_workflows_that_clone_vcpkg_check_out_the_repository_first(self):
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            text = workflow.read_text()
            if "Microsoft/vcpkg.git" in text:
                self.assertLess(text.index("actions/checkout"), text.index("Microsoft/vcpkg.git"),
                                f"{workflow.name}: .vcpkg-commit is read before the checkout")

    def test_the_workflows_that_link_provide_the_case_folded_crypt32(self):
        # The pinned ixwebsocket port links -lCrypt32 and Arch ships libcrypt32.a only.
        for name in ("build.yml", "defender-scan.yml"):
            self.assertIn("ln -s libcrypt32.a /usr/x86_64-w64-mingw32/lib/libCrypt32.a",
                          (WORKFLOWS / name).read_text(), name)

    def test_the_local_pin_check_also_requires_the_case_folded_crypt32(self):
        justfile = (REPO / "justfile").read_text()
        recipe = justfile[justfile.index("vcpkg-pin-check:"):]
        recipe = recipe[:recipe.index("\n\n")]
        self.assertIn("libCrypt32.a", recipe)
        self.assertIn("exit 1", recipe)

    @unittest.skipUnless(shutil.which("just") and (pathlib.Path.home() / ".vcpkg/.git").exists(),
                         "needs just and a vcpkg checkout at ~/.vcpkg")
    def test_the_pin_check_fails_when_the_symlink_is_missing_and_passes_when_present(self):
        head = subprocess.run(["git", "-C", str(pathlib.Path.home() / ".vcpkg"), "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        if head != (REPO / ".vcpkg-commit").read_text().strip():
            self.skipTest("~/.vcpkg is not on the pinned revision")
        with tempfile.TemporaryDirectory() as lib:
            env = dict(os.environ, NEVR_MINGW_LIB=lib)
            missing = subprocess.run(["just", "vcpkg-pin-check"], cwd=REPO, env=env, capture_output=True, text=True)
            self.assertNotEqual(missing.returncode, 0, missing.stdout + missing.stderr)
            self.assertIn("libCrypt32.a is missing", missing.stderr)
            pathlib.Path(lib, "libcrypt32.a").write_bytes(b"")
            os.symlink("libcrypt32.a", os.path.join(lib, "libCrypt32.a"))
            present = subprocess.run(["just", "vcpkg-pin-check"], cwd=REPO, env=env, capture_output=True, text=True)
            self.assertEqual(present.returncode, 0, present.stdout + present.stderr)


if __name__ == "__main__":
    unittest.main()
