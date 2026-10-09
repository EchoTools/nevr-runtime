"""CI builds the vcpkg revision recorded in .vcpkg-commit, read from that one file."""
from __future__ import annotations

import pathlib
import re
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


if __name__ == "__main__":
    unittest.main()
