"""CI builds the vcpkg revision recorded in .vcpkg-commit, read from that one file."""
from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

import yaml

REPO =pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github/workflows"
# A `vcpkg install` command: the binary quoted or not, a line break (a `\\` continuation) or options between the
# binary and `install`. Comment lines are dropped first. Not covered: an install spelled through a variable or a
# wrapper script, or one in a file outside SCANNED.
INSTALL = re.compile(r"vcpkg\"?(?:\s|\\\n)+(?:-\S+(?:\s|\\\n)+)*install\b")
SCANNED = [*sorted(WORKFLOWS.glob("*.yml")), *sorted((REPO / ".github/actions").glob("**/*.yml")),
           *sorted((REPO / "tools").glob("*.sh"))]


def code_of(path):
    return "\n".join(l for l in path.read_text().splitlines() if not l.lstrip().startswith("#"))


def unpinned_installs(text, pin):
    """Every `vcpkg install` in `text` that does not directly follow `env PATH=<pin>` (on this line or a continued one)."""
    before = re.escape("env PATH=" + pin) + r'(?:\s|\\\n)+"?[^\s"]*$'
    return [m.group(0) for m in INSTALL.finditer(text) if not re.search(before, text[:m.start()])]


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

    @staticmethod
    def justfile_pin():
        text = (REPO / "justfile").read_text()
        body = text[text.index("_vcpkg-mingw:"):]
        match = re.search(r"env PATH=(\S+) \./vcpkg install --triplet=x64-mingw-static", body)
        assert match, "justfile _vcpkg-mingw no longer pins PATH for vcpkg install"
        return match.group(1)

    def test_the_justfile_and_the_workflows_pin_the_same_path(self):
        # Run 39433944012: the workflow pinned /usr/local/sbin:...:/bin and the justfile /usr/local/bin:/usr/bin, so
        # `just verify` rebuilt all 14 mingw packages (about 19 minutes of a 33 minute Verify step).
        pins = set()
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            pins.update(re.findall(r"env PATH=(\S+) \\\n\s+\"\$HOME/\.vcpkg/vcpkg\" install", workflow.read_text()))
        self.assertEqual(pins, {self.justfile_pin()})

    def test_every_vcpkg_install_runs_under_the_same_pinned_path(self):
        # x64-mingw-static lists PATH in VCPKG_ENV_PASSTHROUGH, so PATH is hashed into every mingw package
        # ABI. Two jobs with different PATHs name every archive differently and the restore finds none
        # (runs 38424698584 vs 38429980090: all 14 mingw ABIs differed, the 5 x64-linux ones matched).
        # The pin is the justfile's (_vcpkg-mingw), the one `just verify` installs under: one source, no copy.
        # Every install match, not one per file, must directly follow the pin (a second, unpinned install in a
        # file that also has the pinned one is the hole this closes).
        pin = self.justfile_pin()
        installing = [p for p in SCANNED if INSTALL.search(code_of(p))]
        self.assertGreaterEqual(len([p for p in installing if p.parent == WORKFLOWS]), 3,
                                "expected build, vcpkg-cache and defender-scan to install")
        for path in installing:
            self.assertEqual(unpinned_installs(code_of(path), pin), [],
                             f"{path.relative_to(REPO)} runs vcpkg install under the job's own PATH")

    def test_the_install_check_sees_a_second_unpinned_install_and_every_spelling_it_claims(self):
        pin = self.justfile_pin()
        good = f'env PATH={pin} \\\n  "$HOME/.vcpkg/vcpkg" install --triplet=x'
        self.assertEqual(unpinned_installs(good, pin), [])
        self.assertEqual(unpinned_installs(good + "\n" + good, pin), [])
        self.assertEqual(len(unpinned_installs(good + '\n"$HOME/.vcpkg/vcpkg" install --x', pin)), 1)
        self.assertEqual(len(unpinned_installs("$HOME/.vcpkg/vcpkg install", pin)), 1)
        self.assertEqual(len(unpinned_installs('"$HOME/.vcpkg/vcpkg" \\\n  --x-foo=1 install', pin)), 1)
        self.assertEqual(len(unpinned_installs("env PATH=/other \\\n  vcpkg install", pin)), 1)

    def test_no_job_that_installs_vcpkg_changes_path(self):
        # `just verify` runs vcpkg install again (justfile _vcpkg-mingw) under the job's PATH, and PATH is
        # hashed into every mingw package ABI. A job whose PATH differs from the install step's pinned one
        # removes and rebuilds all 14 packages inside the Verify step. The image ships the pinned just.
        # Blind spots, not claimed: an assignment on its own line (`PATH=x` then a later command), `$GITHUB_ENV`
        # and a step's `env:` block; a job that sets PATH those ways still passes this test.
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            text = workflow.read_text()
            if not INSTALL.search(code_of(workflow)):
                continue
            for line in text.splitlines():
                code = line.split("#", 1)[0]
                self.assertIsNone(re.search(r"(?<![\w$])PATH=\S*\s+(?:\S+\s+)*just\b", code),
                                  f"{workflow.name} runs just under its own PATH: {line.strip()}")
                self.assertIsNone(re.search(r"\bexport\s+PATH=", code), f"{workflow.name} exports PATH: {line.strip()}")
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

    def test_the_local_pin_check_also_requires_the_case_folded_crypt32(self):
        justfile = (REPO / "justfile").read_text()
        recipe = justfile[justfile.index("vcpkg-pin-check:"):]
        recipe = recipe[:recipe.index("\n\n")]
        self.assertIn("libCrypt32.a", recipe)
        self.assertIn("exit 1", recipe)

    @unittest.skipUnless(shutil.which("just") and os.environ.get("VCPKG_ROOT")
                         and pathlib.Path(os.environ.get("VCPKG_ROOT", ""), ".git").exists(),
                         "needs just and a vcpkg checkout at $VCPKG_ROOT")
    def test_the_pin_check_fails_when_the_symlink_is_missing_and_passes_when_present(self):
        head = subprocess.run(["git", "-C", os.environ["VCPKG_ROOT"], "rev-parse", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        if head != (REPO / ".vcpkg-commit").read_text().strip():
            self.skipTest("$VCPKG_ROOT is not on the pinned revision")
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
