"""The core installer is built by its own job, in parallel with the DLL build, and the build job waits for it (#1770).

A `needs` edge from build to installer would start build only after the installer job ends, so build keeps
`needs: guard` and waits inside a step, polling the jobs API for the installer job's conclusion.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / ".github" / "workflows" / "build.yml"


def job(text: str, name: str) -> str:
    """The text of the job `name` (its key at two-space indent) up to the next job."""
    match = re.search(rf"^  {re.escape(name)}:\n(.*?)(?=^  \S|\Z)", text, re.S | re.M)
    if match is None:
        raise AssertionError(f"build.yml has no `{name}` job")
    return match.group(0)


def step(text: str, name: str) -> str:
    """The text of the `- name: <name>` step, up to the next step or comment at the same indent."""
    match = re.search(rf"^    - name: {re.escape(name)}\n(.*?)(?=^    - |^    # |\Z)", text, re.S | re.M)
    if match is None:
        raise AssertionError(f"build.yml has no `- name: {name}` step")
    return match.group(0)


class InstallerJobAndWait(unittest.TestCase):
    def setUp(self) -> None:
        self.text = BUILD.read_text(encoding="utf-8")
        self.build = job(self.text, "build")

    def test_the_build_job_depends_on_the_guard_only(self) -> None:
        needs = re.search(r"^    needs:(.*)$", self.build, re.M)
        self.assertIsNotNone(needs, "the build job has no needs line")
        self.assertEqual(needs.group(1).strip(), "guard")
        self.assertNotIn("installer", needs.group(1))

    def test_the_build_job_may_read_actions_for_the_wait(self) -> None:
        permissions = re.search(r"^    permissions:\n((?:      .*\n)+)", self.build, re.M)
        self.assertIsNotNone(permissions, "the build job has no permissions block")
        self.assertIn("actions: read", permissions.group(1))

    def test_the_wait_step_polls_with_a_deadline_and_fails_loudly(self) -> None:
        body = step(self.text, "Wait for the core installer job")
        self.assertIn("deadline", body)
        self.assertIn("exit 1", body)
        self.assertIn("sleep 10", body)
        self.assertIn('"$GITHUB_API_URL/repos/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/attempts/$GITHUB_RUN_ATTEMPT/jobs', body)

    def test_the_installer_build_is_in_the_installer_job_and_not_in_the_build_job(self) -> None:
        installer = job(self.text, "installer")
        self.assertIn("- name: Build the core installer for Windows", installer)
        self.assertIn("- name: Install the Rust toolchain for the core installer", installer)
        self.assertNotIn("- name: Build the core installer for Windows", self.build)
        self.assertNotIn("- name: Install the Rust toolchain for the core installer", self.build)

    def test_the_build_job_steps_run_in_the_order_the_installer_is_waited_for_split_and_verified(self) -> None:
        names = [
            "Verify distribution archives",
            "Wait for the core installer job",
            "Download the core installer",
            "Split, strip and link the core installer's symbols",
            "Verify the symbol files match the stripped binaries",
            "Gate and assemble the release tree",
        ]
        positions = []
        for name in names:
            self.assertEqual(self.build.count(f"- name: {name}\n"), 1, name)
            positions.append(self.build.index(f"- name: {name}\n"))
        self.assertEqual(positions, sorted(positions), f"out of order: {names}")

    def test_the_split_step_comes_after_the_dist_target_that_names_the_package(self) -> None:
        dist = self.build.index("- name: Create distribution packages (unsigned)\n")
        split = self.build.index("- name: Split, strip and link the core installer's symbols\n")
        self.assertLess(dist, split)

    def test_a_failed_poll_is_tried_again_and_only_success_ends_the_wait(self) -> None:
        body = step(self.text, "Wait for the core installer job")
        self.assertIn("--retry 3 --retry-all-errors", body)
        self.assertIn("|| c=unreadable", body)
        self.assertIn("running|absent|unreadable) ;;", body)
        self.assertRegex(body, r'case "\$c" in success\) break;;')
        self.assertIn('x["name"]=="installer"', body)
        self.assertIn("deadline=$((SECONDS + 900))", body)

    def test_the_steps_that_use_the_installer_run_only_on_a_version_run(self) -> None:
        for name in ("Wait for the core installer job", "Download the core installer",
                     "Split, strip and link the core installer's symbols"):
            self.assertIn("if: steps.release.outputs.version != ''", step(self.text, name), name)

    def test_the_installer_job_has_a_timeout_and_its_own_name_is_the_key(self) -> None:
        installer = job(self.text, "installer")
        self.assertRegex(installer, r"(?m)^    timeout-minutes: \d+$")
        self.assertNotRegex(installer, r"(?m)^    name:", "the poll matches the job's name, which is its key only without name:")


if __name__ == "__main__":
    unittest.main()
