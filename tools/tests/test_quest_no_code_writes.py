"""#1127: the feature installers are named only by their own files until the sentinel runs Attach (#1132).

The installers below take their hooks through the rows of the build-time patched libr15 (docs/adr/0021) and write no
code page; each needs Attach to have verified the file, and the production steps do not run Attach yet, so they skip
the features. This test pins that by source shape: no step names an installer, and only the feature files call the row
installer.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QUEST = ROOT / "src/quest"

# Each feature installer, and the files that may name it: its own definition and declaration.
INSTALLERS = {
    r"InstallLobbyFullHooks\b": {"social/lobby_full_hooks.cpp", "social/lobby_full_hooks.h"},
    r"InstallUiTextureMips\w*": {"sentinel/ui_texture_mips.cpp", "sentinel/ui_texture_mips.h"},
    r"splash_skip::Install\b|\bnamespace nevr_quest::splash_skip\b": {
        "sentinel/splash_skip_hook.cpp",
        "sentinel/splash_skip_hook.h",
    },
    r"credits_text::InstallCreditsText\b|InstallCreditsTextHook\b|\bbool InstallCreditsText\(\);": {
        "sentinel/credits_text.cpp",
        "sentinel/credits_text.h",
    },
}
# The only files that may call the row installer themselves.
WRITER_FILES = {
    "sentinel/hook_install.h",
    "sentinel/credits_text.cpp",
    "social/lobby_full_hooks.cpp",
    "sentinel/ui_texture_mips.cpp",
    "sentinel/splash_skip_hook.cpp",
}
FEATURES = ["lobby_full", "ui_texture", "splash_skip", "credits_text"]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def sources():
    for path in sorted(QUEST.rglob("*")):
        rel = path.relative_to(QUEST).as_posix()
        if path.suffix in (".cpp", ".h") and not rel.startswith("tests/"):
            yield rel, strip_comments(path.read_text())


class NoQuestStepWritesCode(unittest.TestCase):
    def test_no_file_outside_a_feature_names_its_installer(self):
        files = dict(sources())
        self.assertGreater(len(files), 50)  # a sensor that reads nothing watches nothing
        for pattern, owners in INSTALLERS.items():
            for rel, text in files.items():
                if rel in owners:
                    continue
                self.assertIsNone(re.search(pattern, text), f"{rel} reaches {pattern}")

    def test_only_the_dormant_feature_files_call_the_row_installer(self):
        calls = re.compile(r"InstallRowThunk\s*<|\bWriteWord\s*\(")
        found = {rel for rel, text in sources() if calls.search(text)}
        self.assertTrue(found, "the dormant writers disappeared from the scan")
        self.assertLessEqual(found, WRITER_FILES, sorted(found - WRITER_FILES))

    def test_the_sentinel_steps_call_no_protection_change(self):
        for rel in ("integration/production_steps.cpp", "sentinel/entry.cpp"):
            text = strip_comments((QUEST / rel).read_text())
            self.assertNotRegex(text, r"\bmprotect\s*\(|PROT_EXEC", rel)

    def test_each_skipped_feature_logs_the_issue_it_waits_on(self):
        text = (QUEST / "integration/production_steps.cpp").read_text()
        for feature in FEATURES:
            self.assertIn(f'LogCodeWriteSkipped("{feature}");', text)
        self.assertIn('{"waits_on", "#1132"}', text)


if __name__ == "__main__":
    unittest.main()
