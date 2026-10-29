#!/usr/bin/env python3
"""Fail when the runtime GTest declarations fall below the count at the base commit.

Counts the lines of src/runtime/tests/*.cpp that begin `TEST(` or `TEST_F(` (an indented, commented or TEST_P
declaration is not counted) in the working tree, and the same files at a base commit read from git: the merge-base
with origin/main, or the parent commit when HEAD is that merge-base (on main itself). The floor is that count; no
number is stored anywhere, so a pull request that adds tests edits nothing to pass (#1343). A deliberate removal is
declared with a `GTest-Drop: <reason>` trailer on a commit after the base, and the reason is printed.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TESTS_DIR = Path("src") / "runtime" / "tests"
DECLARATION = re.compile(r"^TEST(_F)?\(")
DROP_TRAILER = "GTest-Drop"


def count_lines(lines) -> int:
    return sum(1 for line in lines if DECLARATION.match(line))


def count_worktree(directory: Path) -> int:
    total = 0
    for path in sorted(directory.glob("*.cpp")):
        with path.open(encoding="utf-8", errors="replace") as handle:
            total += count_lines(handle)
    return total


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout


def default_base(repo: Path) -> str:
    head = git(repo, "rev-parse", "HEAD").strip()
    base = git(repo, "merge-base", "HEAD", "origin/main").strip()
    if base == head:
        base = git(repo, "rev-parse", "HEAD~1").strip()
    return base


def count_at(repo: Path, rev: str, rel_dir: Path) -> int:
    names = git(repo, "ls-tree", "--name-only", f"{rev}:{rel_dir.as_posix()}").split()
    total = 0
    for name in names:
        if name.endswith(".cpp"):
            text = git(repo, "show", f"{rev}:{(rel_dir / name).as_posix()}")
            total += count_lines(text.splitlines())
    return total


def drop_reasons(repo: Path, base: str) -> list:
    out = git(repo, "log", f"--format=%(trailers:key={DROP_TRAILER},valueonly)", f"{base}..HEAD")
    return [line.strip() for line in out.splitlines() if line.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--base", help="commit to compare with (default: merge-base with origin/main, else HEAD~1)")
    args = parser.parse_args()

    directory = args.repo / TESTS_DIR
    if not directory.is_dir() or not any(directory.glob("*.cpp")):
        print(f"verify: FAIL — no runtime test sources in {directory}; the GTest count cannot be taken.", file=sys.stderr)
        return 1
    try:
        base = args.base or default_base(args.repo)
        base = git(args.repo, "rev-parse", "--verify", f"{base}^{{commit}}").strip()
        floor = count_at(args.repo, base, TESTS_DIR)
    except subprocess.CalledProcessError as error:
        print(f"verify: FAIL — the base commit's GTest count cannot be read: {' '.join(error.cmd)}: "
              f"{error.stderr.strip()}", file=sys.stderr)
        return 1
    count = count_worktree(directory)
    if count < floor:
        reasons = drop_reasons(args.repo, base)
        if not reasons:
            print(f"verify: FAIL — runtime GTest count fell to {count} (base {base[:12]} had {floor}). A deliberate "
                  f"removal carries a '{DROP_TRAILER}: <reason>' trailer on a commit after the base.", file=sys.stderr)
            return 1
        print(f"verify: runtime GTest declarations={count} (base {base[:12]} had {floor}; "
              f"{DROP_TRAILER}: {'; '.join(reasons)})")
        return 0
    print(f"verify: runtime GTest declarations={count} (base {base[:12]} had {floor})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
