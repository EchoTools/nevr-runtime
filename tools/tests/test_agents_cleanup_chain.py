"""AGENTS.md says a cleanup chain stops at its first failed step (#897) and removes a worktree through the recipe (#942)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BULLET = "- **Remove a worktree only after proving it is redundant.**"
RECIPE = "- **Remove a worktree with `just remove-worktree <absolute path>`**"


def bullet(start_marker: str) -> str:
    text = (ROOT / "AGENTS.md").read_text()
    start = text.index(start_marker)
    ends = [i for i in (text.find("\n- **", start + 1), text.find("\n## ", start + 1), text.find("\n### ", start + 1)) if i != -1]
    return text[start:min(ends)]


def removal_bullet() -> str:
    return bullet(BULLET)


class CleanupChainRule(unittest.TestCase):
    def test_the_removal_bullet_joins_its_steps_so_a_failed_step_stops_every_delete_after_it(self):
        text = " ".join(removal_bullet().split())  # the prose wraps; the words are what the rule is
        self.assertIn("`&&`", text)
        self.assertIn("`set -e`", text)
        self.assertIn("never `;`", text)
        self.assertIn("stops every delete after it", text)
        self.assertIn("a delete is never the step after an unchecked one", text)

    def test_the_removal_is_written_as_one_chain_through_the_recipe(self):
        chain = re.search(r"```sh\n(?P<c>.*?)```", removal_bullet(), re.S)
        self.assertIsNotNone(chain, "the bullet carries the chain to copy")
        steps = [s.strip() for s in chain.group("c").replace("\\\n", " ").split("&&")]
        names = ["just remove-worktree", "branch -D"]
        positions = [next((i for i, s in enumerate(steps) if n in s), None) for n in names]
        self.assertNotIn(None, positions, f"a step is missing from the chain: {steps}")
        self.assertEqual(positions, sorted(positions), "remove the worktree, then delete the branch")
        self.assertNotIn(";", chain.group("c").replace("\\;", ""), "no step is joined with a semicolon")
        for banned in ("deinit", "--force", "rm -rf", "worktree remove"):
            self.assertNotIn(banned, chain.group("c"), f"the chain must not run {banned}: the recipe does the removal")

    def test_the_rules_forbid_deinit_and_force_and_name_the_recipe(self):
        text = " ".join(removal_bullet().split())
        self.assertIn("Never `--force`, never `git submodule deinit`", text)
        recipe = " ".join(bullet(RECIPE).split())
        self.assertIn("`tools/remove_worktree.py`", recipe)
        self.assertIn("no `git submodule deinit` and no `--force`", recipe)

    def test_the_recipe_and_the_script_the_rules_name_exist_and_the_reaper_and_the_skill_use_them(self):
        justfile = (ROOT / "justfile").read_text()
        self.assertRegex(justfile, r"(?m)^remove-worktree \*args:\n    python3 -I tools/remove_worktree\.py \{\{ args \}\}$")
        self.assertTrue((ROOT / "tools/remove_worktree.py").is_file())
        reaper = (ROOT / "tools/reap_merged.py").read_text()
        self.assertIn("import remove_worktree as rw", reaper)
        self.assertIn("rw.remove(path, log)", reaper)
        skill = (ROOT / ".claude/skills/merge-cleanup/SKILL.md").read_text()
        self.assertIn("same as\n   `just remove-worktree <path>`", skill)
        self.assertIn("To remove one worktree by hand: `just remove-worktree <absolute path>`", skill)
        self.assertNotIn("deinit it by hand", skill)


if __name__ == "__main__":
    unittest.main()
