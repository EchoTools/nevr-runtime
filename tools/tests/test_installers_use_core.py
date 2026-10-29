"""ADR 0023, test 6: no installer in the tree carries a release hash, an artifact URL or a version of its own.

Every installer takes what to install from the signed release manifest through `nevr-install` (installer/), so a
64-hex digest, a host name that serves artifacts or a release version written into an installer script is a second
source of truth that the manifest chain never checks.
"""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
INSTALLERS = [
    "tools/package-release/install.ps1",
    "tools/quest-install.sh",
    "tools/quest-install.ps1",
]
HEX64 = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{64}(?![0-9A-Fa-f])")
URL = re.compile(r"https?://[^\s'\"`)>]+")
# A dotted version of three numbers ("5.16.1"). Four-part Windows file versions are not release versions, and the
# regex needs a digit on each side so "1.0" in a user-agent or a PowerShell "Version 2" is not matched.
VERSION = re.compile(r"(?<![0-9.])[0-9]+\.[0-9]+\.[0-9]+(?![0-9.])")
RECIPE = re.compile(r"^quest-install\b.*?(?=^\S[^\n]*:\s*$|\Z)", re.M | re.S)


def recipe_text() -> str:
    """The `quest-install` recipe of the justfile: its signature line and its body."""
    lines = (REPO / "justfile").read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith("quest-install"))
    end = start + 1
    while end < len(lines) and (lines[end].startswith((" ", "\t")) or not lines[end].strip()):
        end += 1
    return "\n".join(lines[start:end])


def sources() -> dict[str, str]:
    found = {name: (REPO / name).read_text(encoding="utf-8") for name in INSTALLERS}
    found["justfile:quest-install"] = recipe_text()
    return found


def code_lines(text: str) -> list[tuple[int, str]]:
    """Lines that are not comments: a comment may say what a hash is for; code may not hold one."""
    return [(n, line) for n, line in enumerate(text.splitlines(), 1)
            if not line.lstrip().startswith("#")]


class InstallersUseTheCoreTest(unittest.TestCase):
    def test_no_installer_carries_a_release_hash(self):
        for name, text in sources().items():
            for n, line in code_lines(text):
                self.assertIsNone(HEX64.search(line), f"{name}:{n} carries a 64-hex digest: {line.strip()}")

    def test_no_installer_names_an_artifact_url(self):
        for name, text in sources().items():
            for n, line in code_lines(text):
                self.assertIsNone(URL.search(line), f"{name}:{n} carries a URL: {line.strip()}")

    def test_no_installer_carries_a_release_version(self):
        for name, text in sources().items():
            for n, line in code_lines(text):
                self.assertIsNone(VERSION.search(line), f"{name}:{n} carries a version: {line.strip()}")

    def test_every_installer_calls_the_core(self):
        want = {"tools/package-release/install.ps1": "nevr-install.exe", "tools/quest-install.sh": "nevr-install",
                "tools/quest-install.ps1": "nevr-install.exe", "justfile:quest-install": "nevr-install"}
        for name, text in sources().items():
            code = "\n".join(line for _, line in code_lines(text))
            self.assertTrue(want[name] in code, f"{name} does not call {want[name]}")

    def test_no_player_facing_installer_passes_the_dev_artifact_option(self):
        # --artifact-dir installs a local APK nobody signed for: only the developer tools (quest-install.sh, the just
        # recipe) may pass it. The PowerShell installers are what a player can be handed.
        for name in ("tools/package-release/install.ps1", "tools/quest-install.ps1"):
            text = (REPO / name).read_text()
            for needle in ("artifact-dir", "ArtifactDir", "artifact_dir"):
                self.assertFalse(needle in text, f"{name} mentions {needle}")

    def test_the_quest_installers_take_no_hash_or_url_arguments(self):
        sh = (REPO / "tools/quest-install.sh").read_text()
        ps1 = (REPO / "tools/quest-install.ps1").read_text()
        for needle in ("data_url", "data_sha256", "DATA_SHA256", "DATA_URL"):
            self.assertFalse(needle in sh, f"quest-install.sh still mentions {needle}")
        for needle in ("$DataUrl", "$DataSha256", "$ApkSha256"):
            self.assertFalse(needle in ps1, f"quest-install.ps1 still mentions {needle}")
        self.assertTrue("--yes-uninstall" in sh, "quest-install.sh has no --yes-uninstall")
        self.assertTrue("--yes-uninstall" in ps1, "quest-install.ps1 has no --yes-uninstall")


if __name__ == "__main__":
    unittest.main()
