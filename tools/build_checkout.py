#!/usr/bin/env python3
"""The fixed build checkout (#1736): builds that are cached come from one clean directory that never moves.

A compiler cache hits when the same compile command and the same bytes come round again. The Quest build puts the directory it ran
in into its debug info, and a worktree is a different directory every time, so an object cached from one worktree is wrong in
another and the Quest build ran without the cache. One checkout at one path, built only when `git status --porcelain` is empty,
has the same path and the same inputs on every build, so the cache hits.

  build_checkout.py path                  print the fixed checkout's path
  build_checkout.py is-fixed              exit 0 when the current directory's checkout is the fixed one
  build_checkout.py guard                 exit 1, naming what is dirty, when this is the fixed checkout and it is not clean
  build_checkout.py quest-launcher L      print the compiler launcher L when this is the clean fixed checkout, else nothing
  build_checkout.py sync [REV]            check REV out in the fixed checkout (default origin/main), creating the checkout the
                                          first time; refuses when the checkout is dirty

The path is $NEVR_BUILD_CHECKOUT, else <work root>/build-checkout/nevr-runtime (work root: $NEVR_WORK_ROOT, else
~/src/nevr-runtime-work). Every build recipe's guard and the Quest launcher decision read it from here, so there is one rule.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

MAX_SHOWN = 20
LOCK_REASON = "the fixed build checkout (#1736): builds run here only while it is clean; do not edit files here"
README = """# build-checkout

What: the one fixed, clean checkout the cached builds run in (`tools/build_checkout.py`, issue #1736).
Whose: the merge foreman's builds; nobody edits it. Work happens in a worktree, never here.
Removed: when the project stops building from a fixed path; `git worktree remove` it from the main checkout, by exact path.

`nevr-runtime/` is a detached worktree of the main checkout. `just build-checkout <rev>` moves it to a commit and refuses when
`git status --porcelain` there is not empty. The Quest build uses the compiler cache only from this path.
"""


def work_root():
    return Path(os.environ.get("NEVR_WORK_ROOT") or Path.home() / "src" / "nevr-runtime-work")


def fixed_path():
    given = os.environ.get("NEVR_BUILD_CHECKOUT")
    path = Path(given) if given else work_root() / "build-checkout" / "nevr-runtime"
    return Path(os.path.abspath(path))


def git(args, cwd, check=True):
    env = {k: v for k, v in os.environ.items() if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")}
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=check, capture_output=True, text=True)


def toplevel(cwd):
    """The real path of the checkout that holds `cwd`, or None outside a repository."""
    done = git(["rev-parse", "--show-toplevel"], cwd, check=False)
    if done.returncode != 0:
        return None
    return Path(os.path.realpath(done.stdout.strip()))


def is_fixed(cwd):
    top = toplevel(cwd)
    return top is not None and top == Path(os.path.realpath(fixed_path()))


def dirty_lines(root):
    """`git status --porcelain` lines of the checkout at `root`; a status that cannot be read counts as dirty."""
    done = git(["status", "--porcelain"], root, check=False)
    if done.returncode != 0:
        return [f"git status failed: {done.stderr.strip()}"]
    return [line for line in done.stdout.splitlines() if line.strip()]


def refusal(root, lines):
    shown = lines[:MAX_SHOWN]
    more = f"\n  ... and {len(lines) - MAX_SHOWN} more" if len(lines) > MAX_SHOWN else ""
    return (
        f"build-checkout: REFUSED: {root} is the fixed build checkout and is not clean "
        f"({len(lines)} line(s) of `git status --porcelain`):\n  " + "\n  ".join(shown) + more
        + "\nBuilds from the fixed checkout run only on a clean tree. Work in a worktree and land the change first."
    )


def cmd_guard(_args):
    cwd = Path.cwd()
    if not is_fixed(cwd):
        return 0
    root = toplevel(cwd)
    lines = dirty_lines(root)
    if lines:
        print(refusal(root, lines), file=sys.stderr)
        return 1
    return 0


def cmd_quest_launcher(args):
    cwd = Path.cwd()
    launcher = args.launcher
    if not launcher:
        return 0
    if not is_fixed(cwd):
        print("build-checkout: Quest compiler cache off: this is not the fixed build checkout "
              f"({fixed_path()}); its debug info would carry this directory", file=sys.stderr)
        return 0
    lines = dirty_lines(toplevel(cwd))
    if lines:
        print(refusal(toplevel(cwd), lines), file=sys.stderr)
        return 1
    print(launcher)
    return 0


def cmd_sync(args):
    path = fixed_path()
    main = toplevel(Path.cwd())
    if main is None:
        print("build-checkout: run sync from inside the repository", file=sys.stderr)
        return 2
    if path.exists():
        lines = dirty_lines(path)
        if lines:
            print(refusal(path, lines), file=sys.stderr)
            return 1
    git(["fetch", "--no-tags", "origin", "main"], main, check=False)
    resolved = git(["rev-parse", "--verify", args.rev + "^{commit}"], main, check=False)
    if resolved.returncode != 0:
        print(f"build-checkout: {args.rev} is not a commit", file=sys.stderr)
        return 2
    sha = resolved.stdout.strip()
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        (path.parent / "README.md").write_text(README)
        made = git(["worktree", "add", "--detach", str(path), sha], main, check=False)
        if made.returncode != 0:
            print(f"build-checkout: git worktree add failed: {made.stderr.strip()}", file=sys.stderr)
            return 1
        git(["worktree", "lock", "--reason", LOCK_REASON, str(path)], main, check=False)
    else:
        moved = git(["checkout", "--detach", sha], path, check=False)
        if moved.returncode != 0:
            print(f"build-checkout: git checkout failed: {moved.stderr.strip()}", file=sys.stderr)
            return 1
    head = git(["rev-parse", "HEAD"], path).stdout.strip()
    print(f"build-checkout: {path} at {head}")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("path")
    sub.add_parser("is-fixed")
    sub.add_parser("guard")
    q = sub.add_parser("quest-launcher")
    q.add_argument("launcher", nargs="?", default="")
    s = sub.add_parser("sync")
    s.add_argument("rev", nargs="?", default="origin/main")
    args = parser.parse_args(argv)
    if args.cmd == "path":
        print(fixed_path())
        return 0
    if args.cmd == "is-fixed":
        return 0 if is_fixed(Path.cwd()) else 1
    return {"guard": cmd_guard, "quest-launcher": cmd_quest_launcher, "sync": cmd_sync}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
