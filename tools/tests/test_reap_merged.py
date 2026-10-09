"""tools/reap_merged.py on throwaway repositories: each refusal and the apply path, with a stub gh."""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/reap_merged.py"


def git(cwd, *args, check=True):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-C", str(cwd), *args],
                          check=check, capture_output=True, text=True)


class ReapMergedTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="reap-merged-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.origin = self.tmp / "origin.git"
        git(self.tmp, "init", "-q", "--bare", "-b", "main", str(self.origin))
        self.main = self.tmp / "main"
        git(self.tmp, "clone", "-q", str(self.origin), str(self.main))
        (self.main / "tools").mkdir()
        shutil.copy(SCRIPT, self.main / "tools/reap_merged.py")
        git(self.main, "checkout", "-q", "-b", "main")
        (self.main / ".gitignore").write_text("build/\n")
        git(self.main, "add", "tools", ".gitignore")
        git(self.main, "commit", "-q", "-m", "init", "--no-gpg-sign")
        git(self.main, "push", "-q", "origin", "main")
        (self.main / ".claude/worktrees").mkdir(parents=True)
        self.ledger_rows = []
        self.gh_prs = {}  # branch -> list of {"number", "headRefOid", "state"}

    def add_worktree(self, name, *, commit=True, push=False, owner="seat-a", lock=True):
        wt = self.main / ".claude/worktrees" / name
        git(self.main, "worktree", "add", "-q", "--no-track", "-b", name, str(wt))
        if commit:
            (wt / f"{name}.txt").write_text(name)
            git(wt, "add", f"{name}.txt")
            git(wt, "commit", "-q", "-m", name, "--no-gpg-sign")
        if push:
            git(wt, "push", "-q", "origin", f"{name}:refs/heads/{name}")
        if lock:
            git(self.main, "worktree", "lock", "--reason", "test", str(wt))
        if owner:
            self.ledger_rows.append(f"| .claude/worktrees/{name} (branch {name}) | {owner} | test | when merged |")
        return wt

    def land(self, name):
        """Merge the branch into origin/main with a merge commit, so the tip becomes an ancestor."""
        git(self.main, "fetch", "-q", "origin")
        git(self.main, "checkout", "-q", "main")
        git(self.main, "merge", "-q", "--no-ff", "--no-gpg-sign", "-m", f"merge {name}", name)
        git(self.main, "push", "-q", "origin", "main")

    def run_tool(self, *args, cwd=None):
        ledger = self.tmp / "ledger.md"
        ledger.write_text("# ledger\n\n| artifact | owner | purpose | remove-when |\n|---|---|---|---|\n"
                          + "\n".join(self.ledger_rows) + "\n")
        gh = self.tmp / "gh"
        gh.write_text("#!/usr/bin/env python3\nimport json,sys\nprs=json.load(open(%r))\n"
                      "b=sys.argv[sys.argv.index('--head')+1]\nprint(json.dumps(prs.get(b,[])))\n" % str(self.tmp / "prs.json"))
        gh.chmod(0o755)
        (self.tmp / "prs.json").write_text(json.dumps(self.gh_prs))
        env = dict(os.environ, REAP_LEDGER=str(ledger), REAP_LOG=str(self.tmp / "log/reap.jsonl"), REAP_GH=str(gh))
        return subprocess.run([sys.executable, "-I", str(self.main / "tools/reap_merged.py"), *args],
                              cwd=cwd or self.tmp, env=env, capture_output=True, text=True, timeout=120)

    def records(self):
        return [json.loads(line) for line in (self.tmp / "log/reap.jsonl").read_text().splitlines()]

    def test_dry_run_changes_nothing_and_applies_every_proof(self):
        merged = self.add_worktree("merged", push=True)
        self.land("merged")
        (merged / "build").mkdir()
        (merged / "build/obj.o").write_text("x")
        self.add_worktree("unmerged")
        dirty = self.add_worktree("dirty")
        (dirty / "scratch.txt").write_text("work")
        self.land("dirty")
        logs = self.add_worktree("logs")
        self.land("logs")
        (logs / ".gitignore").write_text("*.log\n")
        git(logs, "add", ".gitignore")
        git(logs, "commit", "-q", "-m", "ignore", "--no-gpg-sign")
        self.land("logs")
        (logs / "run.log").write_text("evidence")
        self.add_worktree("unledgered", owner=None)
        self.land("unledgered")
        before = git(self.main, "worktree", "list").stdout
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        out = result.stdout
        self.assertIn("REAP  .claude/worktrees/merged", out)
        self.assertIn("build/", out)
        self.assertRegex(out, r"KEEP  \.claude/worktrees/unmerged[^\n]*\n\s+- not merged")
        self.assertRegex(out, r"KEEP  \.claude/worktrees/dirty[^\n]*\n(.*\n)*?\s+- uncommitted or untracked work: \?\? scratch.txt")
        self.assertRegex(out, r"KEEP  \.claude/worktrees/logs[^\n]*\n(.*\n)*?\s+- ignored files that are not build output.*run\.log")
        self.assertRegex(out, r"KEEP  \.claude/worktrees/unledgered[^\n]*\n(.*\n)*?\s+- not in the ledger")
        self.assertEqual(git(self.main, "worktree", "list").stdout, before, "a dry run changed the worktrees")
        self.assertTrue((merged / "build/obj.o").exists())

    def test_a_process_with_its_cwd_inside_blocks_removal(self):
        wt = self.add_worktree("busy")
        self.land("busy")
        proc = subprocess.Popen(["sleep", "30"], cwd=wt)
        self.addCleanup(lambda: (proc.kill(), proc.wait()))
        result = self.run_tool()
        self.assertRegex(result.stdout, rf"KEEP  \.claude/worktrees/busy[^\n]*\n\s+- process cwd inside: {proc.pid} sleep")

    def test_apply_removes_worktree_branches_and_writes_ledger_and_log(self):
        wt = self.add_worktree("done", push=True)
        self.land("done")
        (wt / "build").mkdir()
        (wt / "build/obj.o").write_text("x")
        result = self.run_tool("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wt.exists())
        self.assertEqual(git(self.main, "branch", "--list", "done").stdout.strip(), "")
        # The branch is merged by ancestry but no PR proves it, so origin keeps its branch.
        self.assertIn("done", git(self.main, "ls-remote", "--heads", "origin", "done").stdout)
        self.assertIn("origin branch kept", result.stdout)
        ledger = (self.tmp / "ledger.md").read_text()
        self.assertRegex(ledger, r"\| \.claude/worktrees/done \+ branch done \(tip [0-9a-f]{12}\) \| seat-a \|.*removed ")
        actions = [r["action"] for r in self.records()]
        for a in ("start", "unlock", "worktree-remove", "branch-delete-local", "ledger-row", "end"):
            self.assertIn(a, actions)

    def test_a_squash_merged_pr_with_the_exact_head_is_reaped_and_origin_branch_deleted(self):
        wt = self.add_worktree("squashed", push=True)
        tip = git(wt, "rev-parse", "HEAD").stdout.strip()
        self.gh_prs["squashed"] = [{"number": 7, "headRefOid": tip, "state": "MERGED"}]
        result = self.run_tool("--apply")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(wt.exists())
        self.assertEqual(git(self.main, "ls-remote", "--heads", "origin", "squashed").stdout.strip(), "")
        self.assertIn("origin branch deleted", result.stdout)

    def test_a_merged_pr_with_a_different_head_keeps_the_worktree(self):
        wt = self.add_worktree("later", push=True)
        self.gh_prs["later"] = [{"number": 8, "headRefOid": "0" * 40, "state": "MERGED"}]
        result = self.run_tool("--apply")
        self.assertTrue(wt.exists())
        self.assertIn("different head than the tip", result.stdout)

    def test_a_record_that_cannot_be_written_aborts(self):
        self.add_worktree("x")
        blocker = self.tmp / "log"
        blocker.write_text("a file where the log directory must go")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot open log", result.stderr)


if __name__ == "__main__":
    unittest.main()
