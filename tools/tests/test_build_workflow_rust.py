"""The tag build installs its own Rust toolchain before the core installer step (#1309).

The builder image holds Arch's MinGW GCC and no Rust, so `rustup` in the installer step exited 127 on the
v5.19.0 tag build (run 40790413315). The toolchain is installed by a step in build.yml: a pinned rustup-init,
checked against its SHA-256 before it runs, a pinned toolchain and the Windows target, with the tools put on PATH
for the steps after it. It sits before the step that builds the installer and runs on the same condition.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / ".github" / "workflows" / "build.yml"

INSTALL = "Install the Rust toolchain for the core installer"
BUILD_STEP = "Build the core installer for Windows"


def step(text: str, name: str) -> str:
    """The text of the `- name: <name>` step, up to the next step at the same indent."""
    match = re.search(rf"^    - name: {re.escape(name)}\n(.*?)(?=^    - |^    # |\Z)", text, re.S | re.M)
    if match is None:
        raise AssertionError(f"build.yml has no `- name: {name}` step")
    return match.group(0)


class RustToolchainStep(unittest.TestCase):
    def setUp(self) -> None:
        self.text = BUILD.read_text(encoding="utf-8")
        self.install = step(self.text, INSTALL)
        self.build = step(self.text, BUILD_STEP)

    def test_it_comes_before_the_step_that_builds_the_installer(self) -> None:
        self.assertLess(self.text.index(f"- name: {INSTALL}"), self.text.index(f"- name: {BUILD_STEP}"))

    def test_it_runs_on_the_same_condition_as_the_installer_step(self) -> None:
        self.assertIn("if: steps.release.outputs.version != ''", self.install)
        self.assertIn("if: steps.release.outputs.version != ''", self.build)

    def test_rustup_init_is_a_pinned_version_checked_against_its_sha256_before_it_runs(self) -> None:
        self.assertRegex(self.install, r"rustup/archive/\d+\.\d+\.\d+/x86_64-unknown-linux-gnu/rustup-init")
        digest = re.search(r"\b([0-9a-f]{64})\b\s+rustup-init", self.install)
        self.assertIsNotNone(digest, "no 64-hex SHA-256 for rustup-init")
        check = self.install.index("sha256sum -c")
        self.assertLess(check, self.install.index("./rustup-init -y"))

    def test_the_toolchain_is_pinned_and_has_the_windows_target(self) -> None:
        self.assertRegex(self.install, r"--default-toolchain \d+\.\d+\.\d+\b")
        self.assertIn("--target x86_64-pc-windows-gnu", self.install)

    def test_the_tools_reach_the_later_steps(self) -> None:
        self.assertIn('>> "$GITHUB_PATH"', self.install)
        self.assertIn("/bin", self.install[self.install.index('>> "$GITHUB_PATH"') - 80:self.install.index('>> "$GITHUB_PATH"')])

    def test_the_installer_step_does_not_install_anything_itself(self) -> None:
        self.assertNotIn("rustup ", self.build)
        self.assertIn("cargo build --locked --release --target x86_64-pc-windows-gnu", self.build)


if __name__ == "__main__":
    unittest.main()
