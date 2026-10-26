#!/usr/bin/env python3
"""Remove one linked worktree without `git submodule deinit` and without `--force`.

    tools/remove_worktree.py <worktree>             prove, then remove
    tools/remove_worktree.py --dry-run <worktree>   prove and print the plan; change nothing

Every proof below is made before the first change. Any failed proof prints its reason and exits 1 with the
worktree exactly as it was.
  1. linked:    <worktree> is a linked worktree, not the main checkout;
  2. idle:      no process has its cwd inside it;
  3. clean:     `git status --short` is empty and `git status --short --ignored` lists only build output;
  4. submodules, one proof per gitlink in the index:
     - initialised (a `.git` file in the submodule directory): the file points into THIS worktree's own
       `modules/` directory, the submodule's working tree is clean, and its repository holds no commit that
       is not on a remote (HEAD, every branch and tag and the stash: nothing is lost with the repository);
     - a plain copy (no `.git`, what `just worktree-setup` makes): git cannot see a change inside it, so
       it is compared with the main checkout's copy and must be identical to it;
  5. every repository under the worktree's own `modules/` directory holds nothing that is not on a remote.

Then: unlock; delete each initialised submodule's `.git` file; move `<git dir>/worktrees/<name>/modules` aside
(`git worktree remove` refuses a worktree while that directory exists, and `git status` fails while a `.git`
file points into a missing one, so both go); `git worktree remove` (never `--force`). Removing the worktree
deletes the moved directory with the rest of the worktree's administrative directory.

Why not `git submodule deinit --all`: a submodule's registration (`submodule.<name>.url` and `.active`) lives
in the config shared by every worktree, so deinit in one worktree unregisters the submodule in all of them,
and it stops on a plain copy ("contains local modifications"). This tool never writes the shared config, and
it checks that `git config --get-regexp '^submodule\\.'` is byte-identical after the removal.

Exit status: 0 removed; 1 refused, nothing changed; 2 a step failed after the first change (the message says
what was done and what was put back).

Every decision and action is appended as one JSON line to the log (REMOVE_WORKTREE_LOG, default
$XDG_STATE_HOME/nevr-runtime/remove-worktree.jsonl). A record that cannot be written aborts the run.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys

# Ignored paths that are build output or per-worktree setup, not work. Anything else ignored (a run
# log, a scratch file) blocks removal: move it out first. A plain copy under extern/ is covered by the
# comparison with the main checkout, not by this list. The second line is what the post-checkout hook
# (.worktreeinclude) and `just worktree-setup` copy in from the main checkout, and the pytest cache.
IGNORED_OK = (
    "build/", "dist/", "gen/", ".env", ".nevr-worktree-setup/", ".cache/", "compile_commands.json",
    "extern/",
    ".mcp.json", ".claude/settings.local.json", "config/public-defaults.env", ".pytest_cache/",
)
IGNORED_OK_PARTS = ("__pycache__/",)

# Git reads these before it looks at -C: a stray one would send every call to another repository.
STRIPPED_ENV = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")
MODULES_ASIDE = "modules.removing"
SUBMODULE_CONFIG = ("config", "--get-regexp", r"^submodule\.")


class Refused(Exception):
    """A proof failed; nothing was changed. `reasons` is the list of failed proofs."""

    def __init__(self, reasons):
        super().__init__("; ".join(reasons))
        self.reasons = reasons


class RemoveFailed(RuntimeError):
    """A step failed after the first change."""


def git(*args, cwd=None, check=True):
    env = {k: v for k, v in os.environ.items() if k not in STRIPPED_ENV}
    return subprocess.run(["git", *args], cwd=cwd, check=check, text=True, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class Log:
    def __init__(self, path):
        self.path = path
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            self.f = open(path, "a", encoding="utf-8")
        except OSError as e:
            sys.exit(f"remove-worktree: cannot open log {path}: {e}")

    def write(self, **rec):
        rec = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
               "tool": "remove-worktree", "pid": os.getpid(), **rec}
        try:
            self.f.write(json.dumps(rec, sort_keys=True) + "\n")
            self.f.flush()
        except OSError as e:
            sys.exit(f"remove-worktree: cannot write log record to {self.path}: {e}")


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


def diff_trees(a, b, limit=5):
    """Differences between two directory trees, the top-level `.git` of each ignored (at most `limit`)."""
    diffs = []

    def listing(root):
        found = {}
        for dirpath, dirnames, filenames in os.walk(root):
            rel = os.path.relpath(dirpath, root)
            if rel == ".":
                dirnames[:] = [d for d in dirnames if d != ".git"]
                filenames = [f for f in filenames if f != ".git"]
            for name in dirnames + filenames:
                full = os.path.join(dirpath, name)
                found[os.path.normpath(os.path.join(rel, name))] = full
        return found

    left, right = listing(a), listing(b)
    for rel in sorted(set(left) | set(right)):
        if len(diffs) >= limit:
            break
        l, r = left.get(rel), right.get(rel)
        if l is None or r is None:
            diffs.append(f"{rel}: only in the {'main checkout' if l is None else 'worktree'}")
        elif os.path.islink(l) or os.path.islink(r):
            if not (os.path.islink(l) and os.path.islink(r) and os.readlink(l) == os.readlink(r)):
                diffs.append(f"{rel}: differs (symbolic link)")
        elif os.path.isdir(l) != os.path.isdir(r):
            diffs.append(f"{rel}: a directory on one side, a file on the other")
        elif os.path.isfile(l):
            with open(l, "rb") as lf, open(r, "rb") as rf:
                if lf.read() != rf.read():
                    diffs.append(f"{rel}: contents differ")
    return diffs


def unpushed(gitdir):
    """Commits in this repository that no remote-tracking ref holds, counted from every ref (--all includes HEAD)."""
    r = git(f"--git-dir={gitdir}", "rev-list", "--count", "--all", "--not", "--remotes", check=False)
    if r.returncode != 0:
        return None, r.stderr.strip()
    return int(r.stdout.strip() or "0"), None


def repositories_under(modules):
    """Git directories below a worktree's modules/ directory (a submodule's own modules/ is followed)."""
    found = []
    for dirpath, dirnames, filenames in os.walk(modules):
        if "HEAD" in filenames and "objects" in dirnames:
            found.append(dirpath)
            dirnames[:] = [d for d in dirnames if d == "modules"]
    return found


def lock_reason(common, path):
    """None when the worktree is not locked, else its lock reason ("" when none was given)."""
    out = git("worktree", "list", "--porcelain", cwd=os.path.dirname(common)).stdout
    cur = None
    for line in out.splitlines() + [""]:
        if not line:
            cur = None
            continue
        key, _, val = line.partition(" ")
        if key == "worktree":
            cur = os.path.realpath(val)
        elif key == "locked" and cur == path:
            return val
    return None


def submodule_config(common):
    r = git(*SUBMODULE_CONFIG, cwd=os.path.dirname(common), check=False)
    return r.stdout


def plan(path):
    """Make every proof. Return the plan, or raise Refused with every reason that failed."""
    path = os.path.realpath(path)
    if not os.path.isdir(path):
        raise Refused([f"{path} is not a directory"])
    gitdir = os.path.realpath(git("rev-parse", "--absolute-git-dir", cwd=path).stdout.strip())
    common = os.path.realpath(git("rev-parse", "--path-format=absolute", "--git-common-dir", cwd=path).stdout.strip())
    if gitdir == common or os.path.realpath(git("rev-parse", "--show-toplevel", cwd=path).stdout.strip()) != path:
        raise Refused([f"{path} is not the root of a linked worktree (the main checkout and subdirectories are refused)"])
    main = os.path.dirname(common)
    modules = os.path.join(gitdir, "modules")
    reasons = []

    holders = cwd_holders(path)
    if holders:
        reasons.append("process cwd inside: " + ", ".join(holders))

    plain_status, ign = ignored_entries(path)
    if plain_status:
        reasons.append("uncommitted or untracked work: " + "; ".join(plain_status[:8]) + (" ..." if len(plain_status) > 8 else ""))
    odd = [e for e in ign if not is_build_output(e)]
    if odd:
        reasons.append("ignored files that are not build output (move any run log out first): " + ", ".join(odd[:8]))

    initialised, copies = [], []
    for line in git("ls-files", "-s", cwd=path).stdout.splitlines():
        meta, _, name = line.partition("\t")
        if not meta.startswith("160000 "):
            continue
        sub = os.path.join(path, name)
        if not os.path.isdir(sub) or not os.listdir(sub):
            continue
        dotgit = os.path.join(sub, ".git")
        if not os.path.lexists(dotgit):
            diffs = None
            theirs = os.path.join(main, name)
            if not os.path.isdir(theirs):
                reasons.append(f"{name}: a plain copy, and the main checkout has no {name} to compare it with")
            else:
                diffs = diff_trees(sub, theirs)
                if diffs:
                    reasons.append(f"{name}: a plain copy that differs from the main checkout's ({'; '.join(diffs)})")
            copies.append(name)
            continue
        if not os.path.isfile(dotgit) or os.path.islink(dotgit):
            reasons.append(f"{name}: .git is not a plain file (an embedded repository), so nothing proves it is safe to drop")
            continue
        text = open(dotgit, encoding="utf-8").read()
        if not text.startswith("gitdir:"):
            reasons.append(f"{name}: .git does not say 'gitdir: <path>'")
            continue
        target = os.path.realpath(os.path.join(sub, text[len("gitdir:"):].strip()))
        if not target.startswith(os.path.realpath(modules) + os.sep):
            reasons.append(f"{name}: its repository is {target}, not under this worktree's own {modules}")
            continue
        st = git("status", "--porcelain", cwd=sub, check=False)
        if st.returncode != 0:
            reasons.append(f"{name}: git status failed in the submodule: {st.stderr.strip()}")
        elif st.stdout.strip():
            reasons.append(f"{name}: uncommitted or untracked work inside the submodule: " + "; ".join(st.stdout.splitlines()[:5]))
        initialised.append({"name": name, "gitfile": dotgit, "gitfile_text": text, "gitdir": target})

    repos = repositories_under(modules) if os.path.isdir(modules) else []
    for repo in repos:
        n, why = unpushed(repo)
        label = os.path.relpath(repo, modules)
        if n is None:
            reasons.append(f"{label}: cannot count commits that are on no remote: {why}")
        elif n:
            reasons.append(f"{label}: {n} commit(s) in its repository are on no remote (they would be lost with it)")

    if reasons:
        raise Refused(reasons)
    return {"path": path, "gitdir": gitdir, "common": common, "main": main, "modules": modules,
            "initialised": initialised, "plain_copies": copies, "repositories": repos,
            "lock": lock_reason(common, path)}


def remove(path, log=None, dry_run=False):
    """Prove, then remove. Returns a list of what was done; raises Refused or RemoveFailed."""
    say = log.write if log else (lambda **kw: None)
    p = plan(path)
    say(action="plan", worktree=p["path"], initialised=[s["name"] for s in p["initialised"]],
        plain_copies=p["plain_copies"], repositories=p["repositories"], locked=p["lock"] is not None)
    done = []
    if dry_run:
        return [f"would delete the .git file of {s['name']}" for s in p["initialised"]] + \
               ([f"would move {p['modules']} aside"] if os.path.isdir(p["modules"]) else []) + \
               ["would run git worktree remove (no --force)"]
    config_before = submodule_config(p["common"])
    gitfiles, aside = [], None
    try:
        if p["lock"] is not None:
            git("worktree", "unlock", p["path"], cwd=p["main"])
            say(action="unlock", worktree=p["path"])
            done.append("unlocked")
        for s in p["initialised"]:
            os.remove(s["gitfile"])
            gitfiles.append(s)
            say(action="submodule-gitfile-removed", worktree=p["path"], submodule=s["name"], gitdir=s["gitdir"])
            done.append(f"deleted the .git file of {s['name']}")
        if os.path.isdir(p["modules"]):
            aside = os.path.join(p["gitdir"], MODULES_ASIDE)
            os.rename(p["modules"], aside)
            say(action="modules-moved-aside", worktree=p["path"], source=p["modules"], to=aside)
            done.append("moved the worktree's modules directory aside")
        r = git("worktree", "remove", p["path"], cwd=p["main"], check=False)
        if r.returncode != 0:
            raise RemoveFailed(f"git worktree remove: {r.stderr.strip()}")
    except (OSError, subprocess.CalledProcessError, RemoveFailed) as e:
        put_back = []
        if os.path.isdir(p["gitdir"]):
            if aside and os.path.isdir(aside):
                os.rename(aside, p["modules"])
                put_back.append("the modules directory")
            for s in gitfiles:
                if os.path.isdir(os.path.dirname(s["gitfile"])):
                    with open(s["gitfile"], "w", encoding="utf-8") as f:
                        f.write(s["gitfile_text"])
                    put_back.append(f".git file of {s['name']}")
            if p["lock"] is not None:
                git("worktree", "lock", "--reason", p["lock"] or "relocked by remove-worktree after a failed removal",
                    p["path"], cwd=p["main"], check=False)
                put_back.append("the lock")
        detail = e.stderr.strip() if isinstance(e, subprocess.CalledProcessError) else str(e)
        gone = not os.path.isdir(p["gitdir"])
        say(action="failed", worktree=p["path"], error=detail, done=done, put_back=put_back, unregistered=gone)
        note = (f"; git has already unregistered the worktree (it deletes the administrative directory even when the "
                f"worktree directory cannot be deleted), so what is left of {p['path']} is a plain directory to delete "
                f"by hand: every proof held, so nothing in it is unsaved") if gone else ""
        raise RemoveFailed(f"{detail}; done before it: {', '.join(done) or 'nothing'}; "
                           f"put back: {', '.join(put_back) or 'nothing'}{note}")
    say(action="git-worktree-remove", worktree=p["path"])
    done.append("git worktree remove")
    if submodule_config(p["common"]) != config_before:
        say(action="shared-config-changed", worktree=p["path"])
        raise RemoveFailed("the shared submodule config changed during the removal (git config --get-regexp '^submodule\\.' differs); "
                           "the worktree is gone, restore the entries with git submodule init <path>")
    return done


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("worktree")
    ap.add_argument("--dry-run", action="store_true", help="make every proof and print the plan; change nothing")
    args = ap.parse_args()
    state = os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state"))
    log = Log(os.environ.get("REMOVE_WORKTREE_LOG", os.path.join(state, "nevr-runtime", "remove-worktree.jsonl")))
    log.write(action="start", worktree=os.path.realpath(args.worktree), dry_run=args.dry_run)
    try:
        done = remove(args.worktree, log, args.dry_run)
    except Refused as e:
        log.write(action="refused", worktree=os.path.realpath(args.worktree), reasons=e.reasons)
        print(f"remove-worktree: refused, nothing changed: {os.path.realpath(args.worktree)}", file=sys.stderr)
        for r in e.reasons:
            print(f"  - {r}", file=sys.stderr)
        return 1
    except (RemoveFailed, subprocess.CalledProcessError) as e:
        detail = e.stderr.strip() if isinstance(e, subprocess.CalledProcessError) else str(e)
        print(f"remove-worktree: FAILED: {detail}", file=sys.stderr)
        return 2
    for line in done:
        print(("dry-run: " if args.dry_run else "") + line)
    log.write(action="end", worktree=os.path.realpath(args.worktree), dry_run=args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
