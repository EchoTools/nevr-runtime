"""#1492: the core installer is install-nevr-runtime.exe. The old name, nevr-install.exe, survives only as a copy in the
release zip for programs outside this repository that still call it, and only the places listed here may say it.

The data names that happen to start the same way are not the binary and stay: the install record `nevr-install.txt`,
the logs `nevr-install.log` and `nevr-install.jsonl`, and the record's `owner=nevr-install` (a launcher reads them).
"""
from __future__ import annotations

import re
import subprocess
import sys
import tomllib
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NEW = "install-nevr-runtime"
OLD = "nevr-install"

# Files that may say the old name as the binary, and why: the packaging step and its gate (tools/package_release.py,
# the tree-stage verify_symbols.py check, build.yml's cp, the package-dev recipe), the one README.txt sentence, the tester notes' list of the zip's files, and
# RELEASING.md's lifecycle paragraph. Tests are not scanned: they assert on the old name.
OLD_NAME_ALLOWED = {
    ".github/workflows/build.yml",
    "justfile",
    "RELEASING.md",
    "tools/package_release.py",
    "tools/package-release/README.template.txt",
    "tools/package-release/RELEASE-NOTES.template.md",
    "tools/verify_symbols.py",
}
# Not the binary: the data names above, the record owner constant and the refusal text that names the owner, the cargo package (the library and header keep their
# own names), the CLI's per-user state directory and the lockfile entry for the package.
DATA_NAME = re.compile(r'nevr-install\.(?:txt|log|jsonl)\b|owner=nevr-install|OWNER: &str = "nevr-install"|installed by nevr-install(?!\.exe|[\w-])'
                       r'|join\("nevr-install"\)|-ne \'nevr-install\'|^name = "nevr-install"$')


def tracked_text_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, capture_output=True, check=True).stdout.decode().split("\0")
    files = []
    for name in out:
        if not name or name.startswith(("docs/audits/", "tools/tests/", "installer/tests/")) or not (REPO / name).is_file():
            continue
        try:
            (REPO / name).read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        files.append(name)
    return files


class InstallerNameTest(unittest.TestCase):
    def test_the_cargo_binary_is_install_nevr_runtime(self):
        manifest = tomllib.loads((REPO / "installer/Cargo.toml").read_text())
        self.assertEqual([b["name"] for b in manifest["bin"]], [NEW])
        self.assertEqual(manifest["lib"]["name"], "nevr_install", "the library and its C header keep their names")

    def test_the_old_name_as_the_binary_appears_only_where_the_copy_is_made_and_gated(self):
        stray = []
        for name in tracked_text_files():
            if name in OLD_NAME_ALLOWED:
                continue
            for number, line in enumerate((REPO / name).read_text(encoding="utf-8").splitlines(), 1):
                if OLD in DATA_NAME.sub("", line):
                    stray.append(f"{name}:{number}: {line.strip()[:120]}")
        self.assertEqual(stray, [], "the binary is install-nevr-runtime; fix these lines")

    def test_the_owner_message_is_data_but_the_binary_after_the_same_words_is_not(self):
        hidden = ["installed by nevr-install to undo", "installed by nevr-install."]
        flagged = ["installed by nevr-install.exe", "installed by nevr-install.exe pc uninstall",
                   "installed by nevr-install-foo", "run nevr-install.exe installed by nevr-install"]
        for line in hidden:
            self.assertNotIn(OLD, DATA_NAME.sub("", line), line)
        for line in flagged:
            self.assertIn(OLD, DATA_NAME.sub("", line), line)

    def test_the_allowed_files_say_the_old_name_only_for_the_copy(self):
        for name in sorted(OLD_NAME_ALLOWED):
            text = (REPO / name).read_text(encoding="utf-8")
            self.assertIn(OLD, text, f"{name} no longer says the old name: drop it from OLD_NAME_ALLOWED")

    def test_releasing_md_states_the_removal_condition(self):
        text = (REPO / "RELEASING.md").read_text(encoding="utf-8")
        self.assertIn(f"`{OLD}.exe`", text)
        mentions = [p for p in text.split("\n\n") if OLD in p]
        self.assertEqual(len(mentions), 1, "RELEASING.md says the old name in its lifecycle paragraph only")
        paragraph = mentions[0]
        self.assertIn("removed", paragraph)
        for needle in ("first release after", "launcher", "optional", "gate", "this paragraph"):
            self.assertIn(needle, paragraph, needle)


if __name__ == "__main__":
    sys.exit(unittest.main())
