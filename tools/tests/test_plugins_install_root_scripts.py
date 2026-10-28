"""ADR 0012: the scripts that deploy a build's plugins put them in the nevr root's plugins folder (the one the
runtime loads from, #856), never in the game folder's `plugins`. launch-server.sh and verify-server.sh start the
game, so these read their code, comments removed (a sentence that explains the rule cannot satisfy it)."""
from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def code(name: str) -> str:
    lines = (REPO / name).read_text().splitlines()
    return "\n".join(line for line in lines if not re.match(r"^\s*#", line))


class PluginsInstallRootScriptsTest(unittest.TestCase):
    def test_launch_server_deploys_plugins_into_the_nevr_root(self):
        text = code("launch-server.sh")
        self.assertIn('"$NEVR_ROOT_DIR/plugins/"', text)
        self.assertNotIn("$GAME_DIR/plugins", text)

    def test_verify_server_deploys_plugins_into_the_nevr_root(self):
        text = code("verify-server.sh")
        self.assertIn('"$GAME_DIR/$NEVR_ROOT_REL/plugins/', text)
        self.assertNotIn("$GAME_DIR/plugins", text)

    def test_the_go_helpers_use_the_nevr_root_for_plugins(self):
        text = code_go()
        self.assertEqual(text.count('filepath.Join(nevrRootDir(absGameDir), "plugins")'), 2)
        self.assertNotIn('"bin", "win10", "plugins"', text)


def code_go() -> str:
    lines = (REPO / "tests/system/helpers_test.go").read_text().splitlines()
    return "\n".join(line for line in lines if not re.match(r"^\s*//", line))


if __name__ == "__main__":
    unittest.main()
