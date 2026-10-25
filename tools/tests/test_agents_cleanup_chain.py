"""AGENTS.md says a cleanup chain stops at its first failed step (#897)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BULLET = "- **Remove a worktree only after proving it is redundant.**"


def removal_bullet() -> str:
    text = (ROOT / "AGENTS.md").read_text()
    start = text.index(BULLET)
    end = text.index("\n- **", start + 1)
    return text[start:end]


class CleanupChainRule(unittest.TestCase):
    def test_the_removal_bullet_joins_its_steps_so_a_failed_step_stops_every_delete_after_it(self):
        bullet = " ".join(removal_bullet().split())  # the prose wraps; the words are what the rule is
        self.assertIn("`&&`", bullet)
        self.assertIn("`set -e`", bullet)
        self.assertIn("never `;`", bullet)
        self.assertIn("stops every delete after it", bullet)
        self.assertIn("a delete is never the step after an unchecked one", bullet)

    def test_the_submodule_removal_is_written_as_one_chain(self):
        chain = re.search(r"```sh\n(?P<c>.*?)```", removal_bullet(), re.S)
        self.assertIsNotNone(chain, "the bullet carries the chain to copy")
        steps = [s.strip() for s in chain.group("c").replace("\\\n", " ").split("&&")]
        names = ["worktree unlock", "submodule deinit --all", "worktrees/<name>/modules", "worktree remove", "branch -D"]
        positions = [next((i for i, s in enumerate(steps) if n in s), None) for n in names]
        self.assertNotIn(None, positions, f"a step is missing from the chain: {steps}")
        self.assertEqual(positions, sorted(positions), "unlock, deinit, delete modules, remove, delete branch, in that order")
        self.assertNotIn(";", chain.group("c").replace("\;", ""), "no step is joined with a semicolon")
        removal = next(i for i, s in enumerate(steps) if "rm -rf" in s)
        self.assertGreater(removal, next(i for i, s in enumerate(steps) if "submodule deinit" in s),
                           "the modules directory goes only after deinit succeeded")


if __name__ == "__main__":
    unittest.main()
