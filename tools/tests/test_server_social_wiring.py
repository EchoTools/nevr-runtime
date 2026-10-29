"""#1280: a dedicated server is not a player. The social facade's accessor hook asks `Wanted` with the server flag, so a
server keeps the game's null Social provider. `Wanted` itself is tested by src/runtime/tests/test_social_facade.cpp;
the hook is not built into that test, so this source sensor checks the one call that wires it. The check takes the
text it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FACADE = ROOT / "src/runtime/patch/social_facade.cpp"

DECISION = "const bool enabled = Wanted(NevrCfgSocialFacadeEnabled(), g_isServer != FALSE);"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def accessor_problems(source: str) -> list[str]:
    """The accessor hook's body decides the substitution with exactly the server-aware call."""
    code = strip_comments(source)
    match = re.search(r"void\* AccessorHook\(void\* provider\) \{(.*?)\n\}", code, flags=re.S)
    if not match:
        return ["no `void* AccessorHook(void* provider)` in social_facade.cpp"]
    body = match.group(1)
    if body.count(DECISION) != 1:
        return [f"AccessorHook does not decide with `{DECISION}`"]
    if "NevrCfgSocialFacadeEnabled()" in body.replace(DECISION, ""):
        return ["AccessorHook reads social.facade outside the server-aware decision"]
    return []


class AccessorWiring(unittest.TestCase):
    def test_the_tree_passes(self) -> None:
        self.assertEqual(accessor_problems(FACADE.read_text(encoding="utf-8")), [])

    def test_a_decision_without_the_server_flag_fails(self) -> None:
        broken = FACADE.read_text(encoding="utf-8").replace(DECISION, "const bool enabled = NevrCfgSocialFacadeEnabled();")
        self.assertTrue(accessor_problems(broken))

    def test_the_decision_in_a_comment_does_not_count(self) -> None:
        broken = FACADE.read_text(encoding="utf-8").replace(DECISION, "const bool enabled = true;  // " + DECISION)
        self.assertTrue(accessor_problems(broken))


if __name__ == "__main__":
    unittest.main()
