"""tools/git-hooks/post-checkout and tools/install-git-hooks.sh (#667).

A new linked worktree gets the gitignored files `.worktreeinclude` lists, copied from the main
worktree by the tracked post-checkout hook. Each test builds a throwaway repository with a main
checkout, installs the hook with the real installer, and adds a linked worktree with `git worktree
add`. A canary string stands in for the secret a copied file holds; it must never be printed or logged.
"""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
HOOK = REPO / "tools/git-hooks/post-checkout"
INSTALLER = REPO / "tools/install-git-hooks.sh"
CANARY = "CANARY-7f3a9c-do-not-print"
INCLUDE = ".mcp.json\n.claude/settings.local.json\n"
LOG_REL = "nevr-logs/post-checkout.jsonl"


def mode(path: pathlib.Path) -> int:
    return stat.S_IMODE(os.stat(path).st_mode)


class Fixture:
    """A throwaway main checkout with the hook installed through the real installer."""

    def __init__(self, root: pathlib.Path, include: str = INCLUDE, install: bool = True) -> None:
        self.root = root
        self.main = root / "main"
        self.wts = root / "wts"
        self.wts.mkdir()
        self.env = {**os.environ, "HOME": str(root), "GIT_CONFIG_GLOBAL": "/dev/null",
                    "GIT_CONFIG_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0"}
        for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            self.env.pop(var, None)
        self.main.mkdir()
        self.git("init", "-q", "-b", "main", cwd=self.main)
        (self.main / "tools/git-hooks").mkdir(parents=True)
        shutil.copy(HOOK, self.main / "tools/git-hooks/post-checkout")
        shutil.copy(INSTALLER, self.main / "tools/install-git-hooks.sh")
        os.chmod(self.main / "tools/git-hooks/post-checkout", 0o755)
        os.chmod(self.main / "tools/install-git-hooks.sh", 0o755)
        (self.main / ".worktreeinclude").write_text(include)
        (self.main / ".gitignore").write_text(".mcp.json\n.claude/*\n")
        self.commit("init")
        # Branch "blocked" tracks a FILE named .claude, so a worktree on it cannot receive .claude/<x>.
        self.git("checkout", "-q", "-b", "blocked", cwd=self.main)
        (self.main / ".claude").write_text("a file where the directory should be\n")
        self.git("add", "-f", ".claude", cwd=self.main)
        self.commit("blocked")
        self.git("checkout", "-q", "main", cwd=self.main)
        (self.main / ".claude").mkdir()
        for rel in (".mcp.json", ".claude/settings.local.json"):
            target = self.main / rel
            target.write_text(f'{{"token": "{CANARY}"}}\n')
            os.chmod(target, 0o755)  # umask 077 alone would leave 700: only chmod makes it 600
        if install:
            self.install()

    def commit(self, message: str) -> None:
        self.git("add", "-A", cwd=self.main)
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", message,
                 cwd=self.main)

    def git(self, *args: str, cwd: pathlib.Path) -> subprocess.CompletedProcess:
        return subprocess.run(["git", *args], cwd=cwd, env=self.env, capture_output=True, text=True)

    def install(self) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(self.main / "tools/install-git-hooks.sh")], cwd=self.main,
                              env=self.env, capture_output=True, text=True)

    def add_worktree(self, name: str = "wt", base: str = "main") -> tuple[pathlib.Path, subprocess.CompletedProcess]:
        wt = self.wts / name
        res = self.git("worktree", "add", "--no-track", "-q", "-b", name, str(wt), base, cwd=self.main)
        return wt, res

    def run_hook(self, cwd: pathlib.Path, flag: str = "1") -> subprocess.CompletedProcess:
        hook = self.main / ".git/hooks/post-checkout"
        return subprocess.run([str(hook), "0", "1", flag], cwd=cwd, env=self.env, capture_output=True,
                              text=True)

    @property
    def log(self) -> pathlib.Path:
        return self.main / ".git" / LOG_REL

    def log_lines(self) -> list[dict]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines() if line.strip()]


class HookTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="wti-")
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)

    def test_new_linked_worktree_gets_both_files_at_mode_600(self) -> None:
        fx = Fixture(self.root)
        wt, res = fx.add_worktree()
        self.assertEqual(res.returncode, 0, res.stderr)
        for rel in (".mcp.json", ".claude/settings.local.json"):
            self.assertTrue((wt / rel).is_file(), f"{rel} did not arrive")
            self.assertEqual(mode(wt / rel), 0o600, rel)
        self.assertIn(".mcp.json", res.stderr)
        entry = fx.log_lines()[-1]
        self.assertEqual({p["path"]: p["outcome"] for p in entry["paths"]},
                         {".mcp.json": "copied", ".claude/settings.local.json": "copied"})
        self.assertIn("ts", entry)
        self.assertEqual(os.path.realpath(entry["worktree"]), os.path.realpath(wt))

    def test_present_file_is_untouched(self) -> None:
        fx = Fixture(self.root)
        wt, _ = fx.add_worktree()
        mine = wt / ".mcp.json"
        mine.write_text("my own bytes\n")
        os.chmod(mine, 0o644)
        res = fx.run_hook(wt)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(mine.read_text(), "my own bytes\n")
        self.assertEqual(mode(mine), 0o644)
        outcomes = {p["path"]: p["outcome"] for p in fx.log_lines()[-1]["paths"]}
        self.assertEqual(outcomes[".mcp.json"], "present")

    def test_missing_in_main_does_nothing_and_exits_zero(self) -> None:
        fx = Fixture(self.root)
        os.remove(fx.main / ".mcp.json")
        wt, res = fx.add_worktree()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertFalse((wt / ".mcp.json").exists())
        self.assertTrue((wt / ".claude/settings.local.json").is_file())
        outcomes = {p["path"]: p["outcome"] for p in fx.log_lines()[-1]["paths"]}
        self.assertEqual(outcomes[".mcp.json"], "absent_in_main")
        self.assertEqual(fx.run_hook(wt).returncode, 0)

    def test_canary_never_reaches_output_or_log(self) -> None:
        fx = Fixture(self.root)
        wt, res = fx.add_worktree()
        again = fx.run_hook(wt)
        self.assertTrue((wt / ".mcp.json").is_file())
        for text in (res.stdout, res.stderr, again.stdout, again.stderr, fx.log.read_text()):
            self.assertNotIn(CANARY, text)

    def test_log_stays_out_of_gits_reflog_directory(self) -> None:
        fx = Fixture(self.root)
        wt, res = fx.add_worktree()
        self.assertEqual(res.returncode, 0, res.stderr)
        reflog_dir = fx.main / ".git/logs"
        stray = [str(p) for p in reflog_dir.rglob("*") if "nevr" in p.name] if reflog_dir.exists() else []
        self.assertEqual(stray, [], "the hook wrote under <common>/logs/, where git keeps reflogs")
        listing = fx.git("reflog", "list", cwd=fx.main)
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertNotIn("nevr", listing.stdout)
        self.assertTrue(fx.log.is_file(), "the hook wrote no log")

    def test_checkout_in_main_worktree_copies_nothing(self) -> None:
        fx = Fixture(self.root)
        res = fx.git("checkout", "-q", "-b", "other", cwd=fx.main)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertNotIn("nevr-post-checkout", res.stderr)
        self.assertFalse(fx.log.exists(), "the hook acted in the main worktree")
        direct = fx.run_hook(fx.main)
        self.assertEqual(direct.returncode, 0)
        self.assertFalse(fx.log.exists())

    def test_failed_copy_does_not_fail_the_checkout(self) -> None:
        fx = Fixture(self.root)
        wt, res = fx.add_worktree(base="blocked")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(fx.run_hook(wt).returncode, 0)
        outcomes = {p["path"]: p["outcome"] for p in fx.log_lines()[-1]["paths"]}
        self.assertEqual(outcomes[".claude/settings.local.json"], "failed")
        self.assertEqual(outcomes[".mcp.json"], "present")
        self.assertTrue((wt / ".mcp.json").is_file())

    def test_unsafe_lines_are_rejected_and_nothing_is_copied(self) -> None:
        absolute = self.root / "abs-secret.txt"
        absolute.write_text("x\n")
        (self.root / "escape.txt").write_text("x\n")
        include = f"../escape.txt\n{absolute}\n*.json\n# a comment\n\n.mcp.json\n"
        fx = Fixture(self.root, include=include)
        wt, res = fx.add_worktree()
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertFalse((fx.wts / "escape.txt").exists())
        self.assertFalse(pathlib.Path(str(wt) + str(absolute)).exists())
        self.assertTrue((wt / ".mcp.json").is_file())
        outcomes = {p["path"]: p["outcome"] for p in fx.log_lines()[-1]["paths"]}
        self.assertEqual(outcomes["../escape.txt"], "rejected")
        self.assertEqual(outcomes[str(absolute)], "rejected")
        self.assertEqual(outcomes["*.json"], "rejected")
        self.assertEqual(outcomes[".mcp.json"], "copied")
        self.assertEqual(len(outcomes), 4)


class InstallerTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="wti-")
        self.addCleanup(self._tmp.cleanup)
        self.root = pathlib.Path(self._tmp.name)

    def test_installs_a_link_to_the_main_worktrees_hook(self) -> None:
        fx = Fixture(self.root, install=False)
        res = fx.install()
        self.assertEqual(res.returncode, 0, res.stderr)
        link = fx.main / ".git/hooks/post-checkout"
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.path.realpath(link), os.path.realpath(fx.main / "tools/git-hooks/post-checkout"))

    def test_idempotent_when_its_own_link_is_there(self) -> None:
        fx = Fixture(self.root)
        again = fx.install()
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertTrue((fx.main / ".git/hooks/post-checkout").is_symlink())

    def test_refuses_a_different_existing_hook(self) -> None:
        fx = Fixture(self.root, install=False)
        existing = fx.main / ".git/hooks/post-checkout"
        existing.write_text("#!/bin/sh\necho mine\n")
        res = fx.install()
        self.assertEqual(res.returncode, 1)
        self.assertIn("post-checkout", res.stderr)
        self.assertFalse(existing.is_symlink())
        self.assertEqual(existing.read_text(), "#!/bin/sh\necho mine\n")


if __name__ == "__main__":
    unittest.main()
