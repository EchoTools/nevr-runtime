#!/usr/bin/env python3
"""Remove the worktrees under .claude/worktrees whose work has landed, each one only after a proof.

    tools/reap_merged.py            dry run: print what would be removed and why the rest is kept
    tools/reap_merged.py --apply    do it
    tools/reap_merged.py --owner S  consider only worktrees the ledger credits to seat S

A worktree is reaped only when every proof holds:
  1. merged:   its branch tip is an ancestor of origin/main, or `gh` reports a MERGED pull request
               whose head is exactly that tip (a squash merge leaves the tip off main);
  2. clean:    `git status --short` is empty;
  3. ignored:  `git status --short --ignored` lists only build output (IGNORED_OK below);
  4. idle:     no process has its cwd inside the worktree;
  5. ledger:   the ledger credits the worktree to a seat (an item not in the ledger is not ours);
  6. no submodule gitdirs: a worktree with initialised submodules is finished by hand.
Anything that fails a proof is listed with the reason and left alone.

For a worktree that passes: print the ignored list, unlock, `git worktree remove` (never --force),
delete the local branch, delete the origin branch when its PR is merged and origin still has the
tip, and append a removal row to the ledger.

Every decision and action is appended as one JSON line to the log (REAP_LOG, default
$XDG_STATE_HOME/nevr-runtime/reap-merged.jsonl). A record that cannot be written aborts the run.

Environment: REAP_LEDGER (default ~/.local/share/repo-hygiene/nevr-runtime-ledger.md), REAP_LOG,
REAP_BASE (default origin/main), REAP_GH (default gh).
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys

# Ignored paths that are build output or per-worktree setup, not work. Anything else ignored (a run
# log, a scratch file) blocks removal: move it out first.
IGNORED_OK = (
    "build/", "dist/", "gen/", ".env", ".nevr-worktree-setup/", ".cache/", "compile_commands.json",
    "extern/",
)
IGNORED_OK_PARTS = ("__pycache__/",)


def git(*args, cwd=None, check=True):
    return subprocess.run(["git", *args], cwd=cwd, check=check, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class Log:
    def __init__(self, path):
        self.path = path
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            self.f = open(path, "a", encoding="utf-8")
        except OSError as e:
            sys.exit(f"reap-merged: cannot open log {path}: {e}")

    def write(self, **rec):
        rec = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
               "tool": "reap-merged", "pid": os.getpid(), **rec}
        try:
            self.f.write(json.dumps(rec, sort_keys=True) + "\n")
            self.f.flush()
        except OSError as e:
            sys.exit(f"reap-merged: cannot write log record to {self.path}: {e}")


def list_worktrees(root):
    out = git("worktree", "list", "--porcelain", cwd=root).stdout
    wts, cur = [], None
    for line in out.splitlines() + [""]:
        if not line:
            if cur:
                wts.append(cur)
            cur = None
            continue
        key, _, val = line.partition(" ")
        if key == "worktree":
            cur = {"path": val, "branch": None, "locked": None, "prunable": False, "head": None}
        elif key == "HEAD":
            cur["head"] = val
        elif key == "branch":
            cur["branch"] = val.removeprefix("refs/heads/")
        elif key == "locked":
            cur["locked"] = val
        elif key == "prunable":
            cur["prunable"] = True
    return wts


def ledger_owners(ledger_text, rel, branch):
    """Seats the ledger's table rows credit with this worktree directory or branch."""
    owners = []
    dir_re = re.compile(r"(?<![\w./-])" + re.escape(rel) + r"(?![\w-])")
    br_re = re.compile(r"(?<![\w./-])" + re.escape(branch) + r"(?![\w./-])") if branch else None
    for line in ledger_text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3 or re.match(r"^:?-+:?$", cells[0]):
            continue
        if dir_re.search(cells[0]) or (br_re and br_re.search(cells[0])):
            # A seat name is one word; other rows name branches or commits in this cell.
            if re.fullmatch(r"[\w-]+", cells[1]) and cells[1] not in owners:
                owners.append(cells[1])
    return owners


def cwd_holders(path):
    real = os.path.realpath(path)
    holders = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            cwd = os.readlink(f"/proc/{pid}/cwd")
        except OSError:
            continue
        if cwd == real or cwd.startswith(real + "/"):
            try:
                comm = open(f"/proc/{pid}/comm").read().strip()
            except OSError:
                comm = "?"
            holders.append(f"{pid} {comm}")
    return holders


def ignored_entries(path):
    lines = git("status", "--short", "--ignored", cwd=path).stdout.splitlines()
    plain = [ln for ln in lines if not ln.startswith("!! ")]
    ign = [ln[3:] for ln in lines if ln.startswith("!! ")]
    return plain, ign


def is_build_output(entry):
    return entry.startswith(IGNORED_OK) or any(p in entry for p in IGNORED_OK_PARTS)


def merged_pr(gh, branch, tip):
    """(number, None) when a MERGED PR has exactly this tip as its head; else (None, why)."""
    try:
        r = subprocess.run([gh, "pr", "list", "--head", branch, "--state", "merged", "--limit", "10",
                            "--json", "number,headRefOid,state"],
                           text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, f"gh unavailable: {e}"
    if r.returncode != 0:
        return None, f"gh failed: {r.stderr.strip()}"
    prs = json.loads(r.stdout or "[]")
    for pr in prs:
        if pr.get("state") == "MERGED" and pr.get("headRefOid") == tip:
            return pr["number"], None
    if prs:
        return None, "merged PR(s) " + ",".join(f"#{p['number']}" for p in prs) + " have a different head than the tip"
    return None, "no merged PR"


def assess(wt, root, base, gh, ledger_text):
    """Return (reasons_kept, facts). Empty reasons means every proof held."""
    path, branch = wt["path"], wt["branch"]
    rel = os.path.relpath(path, root)
    facts = {"worktree": rel, "branch": branch}
    reasons = []
    if wt["prunable"] or not os.path.isdir(path):
        return ["directory is missing (git worktree prune is for a person to run)"], facts
    tip = git("rev-parse", "HEAD", cwd=path).stdout.strip()
    facts["tip"] = tip

    # 1. merged
    anc = git("merge-base", "--is-ancestor", tip, base, cwd=root, check=False).returncode == 0
    pr, why = (None, None)
    if branch:
        pr, why = merged_pr(gh, branch, tip)
    facts.update(ancestor_of_base=anc, merged_pr=pr, pr_note=why)
    if not anc and pr is None:
        reasons.append(f"not merged: tip {tip[:12]} is not in {base} and {why or 'no branch'}")

    # 2/3. clean, ignored
    plain, ign = ignored_entries(path)
    facts["ignored"] = ign
    if plain:
        reasons.append("uncommitted or untracked work: " + "; ".join(plain[:8]) + (" ..." if len(plain) > 8 else ""))
    odd = [e for e in ign if not is_build_output(e)]
    if odd:
        reasons.append("ignored files that are not build output (move any run log out first): " + ", ".join(odd[:8]))

    # 4. idle
    holders = cwd_holders(path)
    if holders:
        reasons.append("process cwd inside: " + ", ".join(holders))

    # 5. ledger
    owners = ledger_owners(ledger_text, rel, branch)
    facts["owners"] = owners
    if not owners:
        reasons.append("not in the ledger (an item not in it is not ours to remove)")

    # 6. submodule gitdirs
    gitdir = git("rev-parse", "--path-format=absolute", "--git-dir", cwd=path).stdout.strip()
    if os.path.isdir(os.path.join(gitdir, "modules")):
        reasons.append("initialised submodules (" + os.path.join(gitdir, "modules") + "): deinit by hand")
    return reasons, facts


def remote_branch_tip(root, branch):
    out = git("ls-remote", "--heads", "origin", f"refs/heads/{branch}", cwd=root).stdout.split()
    return out[0] if out else None


def apply_one(wt, facts, root, ledger, log):
    path, branch, tip = wt["path"], wt["branch"], facts["tip"]
    rel = facts["worktree"]
    if wt["locked"] is not None:
        git("worktree", "unlock", path, cwd=root)
        log.write(action="unlock", **{k: facts[k] for k in ("worktree", "branch", "tip")})
    r = git("worktree", "remove", path, cwd=root, check=False)
    if r.returncode != 0:
        if wt["locked"] is not None:
            git("worktree", "lock", "--reason", wt["locked"] or "relocked by reap-merged after a failed remove", path,
                cwd=root, check=False)
        raise RuntimeError(f"git worktree remove {rel}: {r.stderr.strip()}")
    log.write(action="worktree-remove", worktree=rel, branch=branch, tip=tip)
    note = ["worktree removed"]
    if branch:
        git("branch", "-D", branch, cwd=root)
        log.write(action="branch-delete-local", worktree=rel, branch=branch, tip=tip)
        note.append("local branch deleted")
        if facts["merged_pr"] is not None:
            rtip = remote_branch_tip(root, branch)
            if rtip == tip:
                p = git("push", "origin", f":refs/heads/{branch}", cwd=root)
                print((p.stdout + p.stderr).strip())
                log.write(action="branch-delete-origin", worktree=rel, branch=branch, tip=tip, pr=facts["merged_pr"])
                note.append("origin branch deleted")
            else:
                note.append(f"origin branch kept (origin has {rtip[:12] if rtip else 'none'}, not the tip)")
        else:
            note.append("origin branch kept (no merged PR proves it)")
    row = (f"| {rel}" + (f" + branch {branch}" if branch else "") + f" (tip {tip[:12]}) | {facts['owners'][0]} | "
           f"reaped by tools/reap_merged.py: merged ({'PR #' + str(facts['merged_pr']) if facts['merged_pr'] else 'tip is in base'}) "
           f"| removed {datetime.date.today().isoformat()}: {'; '.join(note)} |\n")
    try:
        with open(ledger, "a", encoding="utf-8") as f:
            f.write(row)
    except OSError as e:
        raise RuntimeError(f"ledger row not written to {ledger}: {e}")
    log.write(action="ledger-row", worktree=rel, ledger=ledger)
    return note


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--apply", action="store_true", help="remove what passes every proof (default: dry run)")
    ap.add_argument("--owner", help="only worktrees the ledger credits to this seat")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    common = git("rev-parse", "--path-format=absolute", "--git-common-dir", cwd=here).stdout.strip()
    root = os.path.dirname(common)
    wt_dir = os.path.join(root, ".claude", "worktrees")
    base = os.environ.get("REAP_BASE", "origin/main")
    gh = os.environ.get("REAP_GH", "gh")
    ledger = os.environ.get("REAP_LEDGER", os.path.expanduser("~/.local/share/repo-hygiene/nevr-runtime-ledger.md"))
    state = os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state"))
    log = Log(os.environ.get("REAP_LOG", os.path.join(state, "nevr-runtime", "reap-merged.jsonl")))
    try:
        ledger_text = open(ledger, encoding="utf-8").read()
    except OSError as e:
        sys.exit(f"reap-merged: cannot read the ledger {ledger}: {e}")

    git("fetch", "origin", cwd=root)
    base_sha = git("rev-parse", base, cwd=root).stdout.strip()
    mode = "apply" if args.apply else "dry-run"
    log.write(action="start", mode=mode, base=base, base_sha=base_sha, owner_filter=args.owner)
    print(f"reap-merged ({mode}) against {base} = {base_sha[:12]}")

    reap, kept, failed = [], [], 0
    for wt in list_worktrees(root):
        if not os.path.realpath(wt["path"]).startswith(os.path.realpath(wt_dir) + os.sep):
            continue
        reasons, facts = assess(wt, root, base, gh, ledger_text)
        if args.owner and args.owner not in facts.get("owners", []):
            continue
        if reasons:
            kept.append((facts, reasons))
            log.write(action="keep", reasons=reasons, **facts)
        else:
            reap.append((wt, facts))
            log.write(action="would-reap" if not args.apply else "reap", **facts)

    for wt, facts in reap:
        print(f"\nREAP  {facts['worktree']}  branch={facts['branch']}  tip={facts['tip'][:12]}  owner={','.join(facts['owners'])}")
        print(f"      proof: {'tip is an ancestor of ' + base if facts['ancestor_of_base'] else 'merged PR #' + str(facts['merged_pr']) + ' has this exact head'}")
        print("      ignored files that go with it: " + (", ".join(facts["ignored"]) or "(none)"))
        if args.apply:
            try:
                print("      done: " + "; ".join(apply_one(wt, facts, root, ledger, log)))
            except (RuntimeError, subprocess.CalledProcessError) as e:
                failed += 1
                detail = e.stderr.strip() if isinstance(e, subprocess.CalledProcessError) else str(e)
                print(f"      FAILED: {detail}", file=sys.stderr)
                log.write(action="failed", worktree=facts["worktree"], error=detail)
    for facts, reasons in kept:
        print(f"\nKEEP  {facts['worktree']}  branch={facts['branch']}")
        for r in reasons:
            print(f"      - {r}")

    print(f"\n{len(reap)} {'reaped' if args.apply else 'would be reaped'}, {len(kept)} kept, {failed} failed")
    if not args.apply and reap:
        print("run with --apply to act")
    log.write(action="end", mode=mode, reaped=len(reap), kept=len(kept), failed=failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
