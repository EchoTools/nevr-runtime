"""The script redirect's nine stock hashes are one list: the combat area's (combat/script_redirect_list.h) and the repack's staging
(tools/internal/questrepack/consts.go combatRedirectHashes, the repack's staging list). The host's registry must also hold all nine (#1737)."""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
COMBAT_LIST = REPO / "src/quest/combat/script_redirect_list.h"
HOST_HEADER = REPO / "src/quest/plugin/script_redirect.h"
JUSTFILE = REPO / "justfile"
REPACK_CONSTS = REPO / "tools/internal/questrepack/consts.go"


def repack_hashes() -> list[str]:
    """The repack's nine redirected hashes, read from the Go list (combatRedirectHashes in consts.go)."""
    block = re.search(r"combatRedirectHashes = \[\]string\{(.*?)\}", REPACK_CONSTS.read_text(encoding="utf-8"), re.S)
    return re.findall(r'"([0-9a-f]{16})"', block.group(1))


class ScriptRedirectListTest(unittest.TestCase):
    def test_the_combat_list_and_the_staging_name_the_same_nine_hashes_in_the_same_order(self):
        block = re.search(r"kHashes\[\]\s*=\s*\{(.*?)\};", COMBAT_LIST.read_text(encoding="utf-8"), re.S)
        self.assertIsNotNone(block, "kHashes not found in the combat area's script_redirect_list.h")
        found = re.findall(r"0x([0-9a-f]{16})ULL", block.group(1))
        self.assertEqual(found, repack_hashes())
        self.assertEqual(len(found), 9)

    def test_the_host_registry_holds_all_nine(self):
        match = re.search(r"kMaxEntries\s*=\s*(\d+)", HOST_HEADER.read_text(encoding="utf-8"))
        self.assertIsNotNone(match, "kMaxEntries not found in the host header")
        self.assertGreaterEqual(int(match.group(1)), len(repack_hashes()))

    def test_the_repack_recipe_collects_the_redirected_names_as_well_as_the_plain_ones(self):
        self.assertIn("lib????????????????_nevr.so", JUSTFILE.read_text(encoding="utf-8"))


def _code(path: pathlib.Path) -> str:
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _body(text: str, signature: str) -> str:
    start = text.index(signature)
    open_at = text.index("{", start)
    depth = 0
    for i in range(open_at, len(text)):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[open_at : i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


class DlopenHookWiringTest(unittest.TestCase):
    """dlopen_hook.cpp's install cannot run on the host (it needs libr15's GOT), so its two calls into the redirect are pinned by
    source shape; the redirect itself runs in script_redirect_test and the handler in integration_hooks_test."""

    SOURCE = _code(REPO / "src/quest/integration/dlopen_hook.cpp")

    def test_the_install_logs_the_redirect_state_once_the_hook_is_live(self):
        install = _body(self.SOURCE, "DlopenInstall InstallDlopenHook(")
        self.assertIn("script_redirect::LogState()", install)
        self.assertGreater(install.index("script_redirect::LogState()"), install.index("g_installed.store(true"))
        self.assertGreater(install.index("script_redirect::LogState()"), install.index("DlopenInstall::kFailed"))

    def test_the_handler_opens_through_the_redirect_and_tells_the_observer_the_games_own_name(self):
        handler = _body(self.SOURCE, "void* HookedDlopen(")
        self.assertIn("script_redirect::OpenRedirected(original, name, flags", handler)
        self.assertNotIn("original(", handler.replace("OpenRedirected(original", ""))
        self.assertIn("AfterDlopen(name, handle)", handler)


if __name__ == "__main__":
    unittest.main()
