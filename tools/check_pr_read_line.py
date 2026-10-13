#!/usr/bin/env python3
"""Check that a PR body ends with `Read AGENTS.md at <sha>`, where <sha> is the PR's base commit.

The base commit is the merge-base of the PR head and the target branch's tip: the commit whose
AGENTS.md the author branched from (see AGENTS.md, "Review, verification and merge"). Any
unambiguous abbreviation of at least 7 hex characters is accepted.

A tell for "did the agent read the contract", not a security control. Standard library only.

    PR_BODY=... check_pr_read_line.py --base-sha <target tip> --head-sha <PR head>
    check_pr_read_line.py --body-file body.txt --base-sha <target tip> --head-sha <PR head> [--repo DIR]

Exit 0 and one `ok:` line on success; exit 1 and one `FAIL:` line on stderr otherwise.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

READ_LINE = re.compile(r"^Read AGENTS\.md at (\S+)$")
HEX = re.compile(r"^[0-9a-fA-F]{7,40}$")


class Failure(Exception):
    pass


def git(repo: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)


def merge_base(repo: str, base: str, head: str) -> str:
    for name, value in (("base", base), ("head", head)):
        if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
            raise Failure(f"--{name}-sha {value!r} is not a full 40-hex commit sha")
    result = git(repo, "merge-base", base, head)
    if result.returncode != 0 or not result.stdout.strip():
        raise Failure(f"cannot compute the merge-base of {base} and {head}: "
                      f"{(result.stderr.strip().splitlines() or ['no common ancestor'])[0]}")
    return result.stdout.strip()


def check(body: str, base: str, head: str, repo: str = ".") -> str:
    """Return the merge-base sha when the body is correct; raise Failure with one line otherwise."""
    lines = [line.rstrip() for line in body.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    non_empty = [line for line in lines if line.strip()]
    if not any(line.startswith("Read AGENTS.md at") for line in non_empty):
        raise Failure("the PR body is missing the `Read AGENTS.md at <sha>` line; end the body with it")
    last = non_empty[-1]
    match = READ_LINE.match(last)
    if match is None:
        if last.startswith("Read AGENTS.md at"):
            raise Failure(f"malformed last line {last!r}; it must be exactly `Read AGENTS.md at <sha>`")
        raise Failure(f"the `Read AGENTS.md at <sha>` line is not the last non-empty line (last is {last!r})")
    claimed = match.group(1)
    if not HEX.match(claimed):
        raise Failure(f"malformed sha {claimed!r} in the read line; use 7 to 40 hex characters")
    actual = merge_base(repo, base, head)
    resolved = git(repo, "rev-parse", "--verify", "--quiet", "--end-of-options", f"{claimed}^{{commit}}")
    resolved_sha = resolved.stdout.strip() if resolved.returncode == 0 else ""
    if not actual.startswith(claimed.lower()):
        raise Failure(f"{claimed} is not the PR's base commit; the merge-base of the head and the target "
                      f"is {actual[:12]}")
    if resolved_sha != actual:
        # The text is a prefix of the base, but git does not read it as the base: a tag or branch of that
        # name wins over the object name, or the prefix is ambiguous between objects.
        where = f"resolves to {resolved_sha[:12]}, not the base" if resolved_sha else "does not resolve to one commit"
        raise Failure(f"{claimed} is a prefix of the PR's base commit {actual[:12]} but is ambiguous: git "
                      f"{where} (a ref or another object shares the name); write the full 40-hex sha")
    return actual


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-sha", required=True, help="tip of the target branch (40 hex)")
    parser.add_argument("--head-sha", required=True, help="head commit of the PR (40 hex)")
    parser.add_argument("--body-file", help="file holding the PR body; default: the PR_BODY environment variable")
    parser.add_argument("--repo", default=".", help="git repository holding both commits")
    args = parser.parse_args(argv)
    try:
        if args.body_file:
            with open(args.body_file, encoding="utf-8") as handle:
                body = handle.read()
        else:
            body = os.environ.get("PR_BODY", "")
        actual = check(body, args.base_sha, args.head_sha, args.repo)
    except (Failure, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"ok: the PR body names its base commit {actual[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
