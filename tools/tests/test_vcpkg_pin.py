"""CI builds the vcpkg revision recorded in .vcpkg-commit, read from that one file."""
from __future__ import annotations

import pathlib
import re
import unittest

import yaml

REPO =pathlib.Path(__file__).resolve().parents[2]
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
            # An action pinned by its own commit SHA (`uses: owner/name@<sha> # vN`) is not the vcpkg pin.
            without_action_pins = re.sub(r"\buses:\s*[\w./-]+@[0-9a-f]{40}\b", "uses:", text)
            self.assertIsNone(re.search(r"\b[0-9a-f]{40}\b", without_action_pins), f"{workflow.name} hard-codes a sha")

    def test_the_workflows_that_clone_vcpkg_check_out_the_repository_first(self):
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            text = workflow.read_text()
            if "Microsoft/vcpkg.git" in text:
                self.assertLess(text.index("actions/checkout"), text.index("Microsoft/vcpkg.git"),
                                f"{workflow.name}: .vcpkg-commit is read before the checkout")

    def test_every_vcpkg_install_runs_under_the_same_pinned_path(self):
        # x64-mingw-static lists PATH in VCPKG_ENV_PASSTHROUGH, so PATH is hashed into every mingw package
        # ABI. Two jobs with different PATHs name every archive differently and the restore finds none
        # (runs 38424698584 vs 38429980090: all 14 mingw ABIs differed, the 5 x64-linux ones matched).
        pinned = "env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        installing = [p for p in sorted(WORKFLOWS.glob("*.yml")) if '"$HOME/.vcpkg/vcpkg" install' in p.read_text()]
        self.assertGreaterEqual(len(installing), 3, "expected build, vcpkg-cache and defender-scan to install")
        for workflow in installing:
            text = workflow.read_text()
            self.assertRegex(text, re.escape(pinned) + r" \\\n\s+\"\$HOME/\.vcpkg/vcpkg\" install",
                             f"{workflow.name} runs vcpkg install under the job's own PATH")

    def test_no_job_that_installs_vcpkg_changes_path(self):
        # `just verify` runs vcpkg install again (justfile _vcpkg-mingw) under the job's PATH, and PATH is
        # hashed into every mingw package ABI. A job whose PATH differs from the install step's pinned one
        # removes and rebuilds all 14 packages inside the Verify step. The image ships the pinned just.
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            text = workflow.read_text()
            if '"$HOME/.vcpkg/vcpkg" install' not in text:
                continue
            self.assertNotIn("GITHUB_PATH", text, f"{workflow.name} edits PATH for later steps")
            self.assertNotIn("extractions/setup-just", text, f"{workflow.name} installs just onto PATH")
        build = (WORKFLOWS / "build.yml").read_text()
        self.assertIn('test "$(just --version)" = "just 1.58.0"', build)

    def test_the_cache_key_prefix_is_the_same_in_the_seed_and_the_consumer(self):
        keys = {}
        for name in ("build.yml", "vcpkg-cache.yml"):
            keys[name] = set(re.findall(r"vcpkg-bin\d+-", (WORKFLOWS / name).read_text()))
        self.assertEqual(len(keys["build.yml"]), 1, keys)
        self.assertEqual(keys["build.yml"], keys["vcpkg-cache.yml"])

    def test_the_cache_seed_runs_on_main_when_a_key_input_changes_and_on_demand(self):
        # A cache is restorable only from the run's own ref and the default branch, so main must hold the key
        # build.yml asks for or every tag and release run compiles the dependencies (~39 min).
        text = (WORKFLOWS / "vcpkg-cache.yml").read_text()
        triggers = yaml.safe_load(text)
        triggers = triggers.get("on", triggers.get(True))
        self.assertEqual(set(triggers), {"push", "workflow_dispatch"})
        self.assertEqual(triggers["push"]["branches"], ["main"])
        # every input of the key: vcpkg.json (hashed), the pin, the toolchain image, and the workflow itself
        for path in ("vcpkg.json", ".vcpkg-commit", ".github/builder/**", ".github/workflows/vcpkg-cache.yml"):
            self.assertIn(path, triggers["push"]["paths"])
        self.assertIn(".vcpkg-commit", text)

    def test_the_builder_image_provides_the_case_folded_crypt32(self):
        # The pinned ixwebsocket port links -lCrypt32 and Arch ships libcrypt32.a only; every
        # workflow that links runs in the builder image, which carries the symlink.
        dockerfile = (REPO / ".github/builder/Dockerfile").read_text()
        self.assertIn("ln -s libcrypt32.a /usr/x86_64-w64-mingw32/lib/libCrypt32.a", dockerfile)
        for name in ("build.yml", "vcpkg-cache.yml", "defender-scan.yml"):
            text = (WORKFLOWS / name).read_text()
            self.assertIn("ghcr.io/echotools/nevr-runtime-builder:", text, name)
            self.assertNotIn("ln -s libcrypt32.a", text, f"{name} re-creates a link the image has")


if __name__ == "__main__":
    unittest.main()
