"""tools/verify_agents_imports.py: AGENTS.md imports the logging standard, and verify runs the check (#1277)."""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "verify_agents_imports.py"
IMPORT = "@docs/standards/logging.md"
SENTENCE = "The logging standard below is part of this contract"


def run(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)], capture_output=True, text=True)


def verify_recipe_code() -> list[str]:
    """The non-comment lines of the `verify` recipe in the justfile."""
    body = just_recipe.body((REPO / "justfile").read_text(encoding="utf-8"), "verify")
    if body is None:
        raise AssertionError("justfile has no verify recipe")
    return [line.strip() for line in body.splitlines() if line.strip() and not line.lstrip().startswith("#")]


class AgentsImportsTest(unittest.TestCase):
    def tree(self) -> Path:
        root = Path(tempfile.mkdtemp(prefix="agents-imports-"))
        self.addCleanup(shutil.rmtree, root, True)
        shutil.copy(REPO / "AGENTS.md", root / "AGENTS.md")
        (root / "docs/standards").mkdir(parents=True)
        shutil.copy(REPO / "docs/standards/logging.md", root / "docs/standards/logging.md")
        return root

    def edit(self, root: Path, old: str, new: str) -> None:
        path = root / "AGENTS.md"
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def assertFails(self, root: Path, message: str) -> None:
        result = run(root)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)

    def test_the_tree_passes(self):
        result = run(REPO)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("agents-imports OK: " + IMPORT, result.stdout)

    def test_a_missing_import_fails(self):
        root = self.tree()
        self.edit(root, IMPORT + "\n", "")
        self.assertFails(root, f"no `{IMPORT}` line")

    def test_an_import_inside_a_code_fence_fails(self):
        root = self.tree()
        self.edit(root, IMPORT + "\n", "```\n" + IMPORT + "\n```\n")
        self.assertFails(root, f"no `{IMPORT}` line")

    def test_an_indented_import_fails(self):
        root = self.tree()
        self.edit(root, IMPORT + "\n", "    " + IMPORT + "\n")
        self.assertFails(root, f"no `{IMPORT}` line")

    def test_an_import_below_the_first_section_fails(self):
        root = self.tree()
        self.edit(root, IMPORT + "\n", "")
        self.edit(root, "## Project\n", "## Project\n\n" + IMPORT + "\n")
        self.assertFails(root, f"no `{IMPORT}` line")

    def test_an_import_of_a_missing_file_fails(self):
        root = self.tree()
        (root / "docs/standards/logging.md").unlink()
        self.assertFails(root, "which does not exist")

    def test_the_sentence_that_makes_it_binding_is_required(self):
        root = self.tree()
        self.edit(root, SENTENCE, "The logging standard")
        self.assertFails(root, f'no "{SENTENCE}"')

    def test_a_tree_without_agents_md_fails(self):
        root = self.tree()
        (root / "AGENTS.md").unlink()
        self.assertFails(root, "cannot read")

    def test_the_default_root_is_this_checkout(self):
        result = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True, cwd=tempfile.gettempdir())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_verify_runs_the_check(self):
        self.assertIn("python3 tools/verify_agents_imports.py", verify_recipe_code())


if __name__ == "__main__":
    unittest.main()
