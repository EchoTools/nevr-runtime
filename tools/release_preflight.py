#!/usr/bin/env python3
"""Refuse to start a release from a clone that is not exactly origin's release branch.

    tools/release_preflight.py [--base main]        (`just release-preflight`)

Run it before building a package or creating any release: before `just package-dev`, before
pushing a v<x.y.z> release tag and before `gh release create`. It exits 0 and prints
`release-preflight: OK ...` only when ALL of these hold; otherwise it exits 1 and prints one
`release-preflight: PROBLEM: ...` line for EACH problem (it never stops at the first):

  1. every local tag is on origin, and points at the same commit there;
  2. there is no uncommitted change (staged or not) and no untracked file (ignored files do not count);
  3. HEAD is on origin's <base> branch (no commit that origin does not have);
  4. this clone is not behind origin's <base> (HEAD is origin's tip exactly);
  5. origin is reachable, and is the project's repository (github.com EchoTools/nevr-runtime, the ssh and
     https forms; `--expect-origin REGEX` overrides): a clone whose `origin` points elsewhere, or a check
     that cannot run, is a refusal, not a pass. Every git command is checked: a non-zero exit, or git
     missing from PATH, is a problem line and a log record, never "clean".

It is READ-ONLY. The git commands it runs, and nothing else:
  git rev-parse --show-toplevel          git config --get remote.origin.url   git rev-parse HEAD
  git status --porcelain=v1 --untracked-files=all
  git for-each-ref refs/tags --format=...  (local tags and their peeled commits)
  git ls-remote origin                   (origin's refs: the only network call; it reads, it writes nothing here)
  git rev-parse --verify --quiet <tip>^{commit}   git rev-list --count <tip>..HEAD   git rev-list --count HEAD..<tip>
  (the verify exits 1, silently, for an absent object: that alone means "not in this clone"; any other
  non-zero exit of it, and any non-zero exit or non-numeric output of rev-list, is a problem line, never
  "nothing to count".)
There is no fetch, pull, push, tag, checkout, reset or remote update. A clone that is behind is told
to `git fetch`; the script never does it for you.

Every run appends one JSON line (time, user, cwd, head, base, origin tip, result, problems) to
$RELEASE_PREFLIGHT_LOG, default $XDG_STATE_HOME/nevr-runtime/release-preflight.jsonl; a record that
cannot be written aborts the run with exit 2.
"""
import argparse
import datetime
import getpass
import json
import os
import re
import subprocess
import sys
from pathlib import Path

MAX_PATHS = 20
# origin must be this repository, in either URL form; the project is public, so this is not a secret.
EXPECT_ORIGIN = r"^(git@github\.com:|https://github\.com/|ssh://git@github\.com/)EchoTools/nevr-runtime(\.git)?/?$"


def git(*args, cwd):
    """Run git. A missing git binary is a failed result (127), never an exception."""
    try:
        return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    except FileNotFoundError:
        return subprocess.CompletedProcess(["git", *args], 127, "", "git is not installed or not on PATH")


def failure(result, what: str) -> str:
    first = (result.stderr.strip().splitlines() or ["no error text"])[0]
    return f"{what} failed (exit {result.returncode}): {first}"


def log_path() -> tuple:
    explicit = os.environ.get("RELEASE_PREFLIGHT_LOG")
    if explicit:
        return Path(explicit), False
    state = os.environ.get("XDG_STATE_HOME") or str(Path.home() / ".local" / "state")
    return Path(state) / "nevr-runtime" / "release-preflight.jsonl", True


def write_log(record: dict) -> None:
    path, create = log_path()
    try:
        if create:
            path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(json.dumps(record, sort_keys=True) + "\n")
    except OSError as error:
        print(f"release-preflight: cannot write the log {path}: {error}", file=sys.stderr)
        print(f"release-preflight: cannot write the log {path}: {error}")
        sys.exit(2)


def parse_ls_remote(text: str) -> tuple:
    """(heads, tags): name -> (object, peeled commit or None) from `git ls-remote` output."""
    heads, tags = {}, {}
    for line in text.splitlines():
        sha, _, ref = line.partition("\t")
        if ref.startswith("refs/heads/"):
            heads[ref[len("refs/heads/"):]] = sha
        elif ref.startswith("refs/tags/"):
            name = ref[len("refs/tags/"):]
            if name.endswith("^{}"):
                obj, _ = tags.get(name[:-3], (None, None))
                tags[name[:-3]] = (obj, sha)
            else:
                _, peeled = tags.get(name, (None, None))
                tags[name] = (sha, peeled)
    return heads, tags


def local_tags(root: str) -> tuple:
    """(tags, failure message or None): name -> (object, peeled commit or None)."""
    out = git("for-each-ref", "refs/tags", "--format=%(refname:strip=2)\t%(objectname)\t%(*objectname)", cwd=root)
    if out.returncode != 0:
        return {}, failure(out, "git for-each-ref refs/tags")
    tags = {}
    for line in out.stdout.splitlines():
        name, obj, peeled = (line.split("\t") + ["", ""])[:3]
        tags[name] = (obj, peeled or None)
    return tags, None


def commit_of(entry: tuple) -> str:
    obj, peeled = entry
    return peeled or obj


def plural(n: int, word: str) -> str:
    return f"{n} {word}" + ("" if n == 1 else "s")


def check(root: str, base: str, expect_origin: str = EXPECT_ORIGIN) -> tuple:
    """Returns (problems, facts). Never raises for a git failure: that is itself a problem."""
    problems, facts = [], {"base": base}
    head_result = git("rev-parse", "HEAD", cwd=root)
    head = head_result.stdout.strip()
    facts["head"] = head
    if head_result.returncode != 0 or not head:
        problems.append(failure(head_result, "git rev-parse HEAD"))

    status_result = git("status", "--porcelain=v1", "--untracked-files=all", cwd=root)
    if status_result.returncode != 0:
        problems.append(failure(status_result, "git status"))
    status = status_result.stdout.splitlines()
    changed = [l[3:] for l in status if not l.startswith("??")]
    untracked = [l[3:] for l in status if l.startswith("??")]
    for label, paths in (("uncommitted change", changed), ("untracked file", untracked)):
        for path in paths[:MAX_PATHS]:
            problems.append(f"{label}: {path}")
        if len(paths) > MAX_PATHS:
            problems.append(f"{len(paths) - MAX_PATHS} more {label}s not listed")

    # The URL as configured (not expanded by url.<base>.insteadOf): what the clone says origin is.
    remote = git("config", "--get", "remote.origin.url", cwd=root)
    if remote.returncode != 0 or not remote.stdout.strip():
        problems.append("no remote named origin: nothing to compare this clone with")
        return problems, facts
    origin_url = remote.stdout.strip()
    facts["origin_url"] = origin_url
    if not re.match(expect_origin, origin_url):
        problems.append(f"origin is {origin_url}, not the project's repository "
                        f"(github.com/EchoTools/nevr-runtime); a clone of another repository proves nothing")
    refs = git("ls-remote", "origin", cwd=root)
    if refs.returncode != 0:
        first = (refs.stderr.strip().splitlines() or ["no error text"])[0]
        problems.append(f"cannot reach origin ({remote.stdout.strip()}): {first}")
        return problems, facts
    heads, remote_tags = parse_ls_remote(refs.stdout)

    tags, tags_failure = local_tags(root)
    if tags_failure:
        problems.append(tags_failure)
    for name, entry in sorted(tags.items()):
        if name not in remote_tags:
            problems.append(f"local tag {name} exists only in this clone (not on origin)")
        elif commit_of(entry) != commit_of(remote_tags[name]):
            problems.append(f"tag {name} points at {commit_of(entry)[:12]} here, "
                            f"{commit_of(remote_tags[name])[:12]} on origin")

    tip = heads.get(base)
    facts["origin_tip"] = tip
    if tip is None:
        problems.append(f"origin has no branch {base}")
    elif head != tip:
        # `git rev-parse --verify --quiet <tip>^{commit}` exits 1, silently, when the object is absent and
        # 128 (or 127 for a missing git) when it could not run: only the first means "not in this clone".
        # (`git cat-file -e` exits 128 for both, so it could not tell them apart.)
        probe = git("rev-parse", "--verify", "--quiet", f"{tip}^{{commit}}", cwd=root)
        if probe.returncode not in (0, 1):
            problems.append(failure(probe, f"git rev-parse --verify {tip[:12]}^{{commit}}"))
        elif probe.returncode == 1:
            problems.append(f"this clone is behind origin/{base}: origin's tip {tip[:12]} is not in this "
                            f"clone (git fetch, then re-run)")
        else:
            counts = []
            for rev_range in (f"{tip}..HEAD", f"HEAD..{tip}"):
                counted = git("rev-list", "--count", rev_range, cwd=root)
                if counted.returncode != 0:
                    problems.append(failure(counted, f"git rev-list --count {rev_range[:12]}.."))
                    break
                if not counted.stdout.strip().isdigit():
                    problems.append(f"git rev-list --count {rev_range[:12]}.. printed "
                                    f"'{counted.stdout.strip()[:40]}', not a number")
                    break
                counts.append(int(counted.stdout.strip()))
            ahead, behind = counts if len(counts) == 2 else (0, 0)
            if ahead:
                problems.append(f"HEAD {head[:12]} is not on origin/{base}: {plural(ahead, 'commit')} "
                                f"not pushed")
            if behind:
                problems.append(f"this clone is behind origin/{base} by {plural(behind, 'commit')} "
                                f"(origin's tip {tip[:12]}; git fetch and update)")
    facts["local_tags"] = len(tags)
    return problems, facts


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--base", default="main", help="origin's release branch (default main)")
    parser.add_argument("--expect-origin", default=EXPECT_ORIGIN, metavar="REGEX",
                        help="regular expression origin's URL must match (default: the project's GitHub URL)")
    args = parser.parse_args(argv)
    top = git("rev-parse", "--show-toplevel", cwd=".")
    if top.returncode != 0:
        root, problems, facts = os.getcwd(), [failure(top, "git rev-parse --show-toplevel")], {}
        if top.returncode != 127:
            problems = ["not inside a git work tree"]
    else:
        root = top.stdout.strip()
        problems, facts = check(root, args.base, args.expect_origin)
    record = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
              "user": getpass.getuser(), "cwd": root, "head": facts.get("head"), "base": args.base,
              "origin_tip": facts.get("origin_tip"), "origin_url": facts.get("origin_url"),
              "expect_origin": args.expect_origin,
              "result": "refused" if problems else "ok", "problems": problems}
    write_log(record)
    for problem in problems:
        print(f"release-preflight: PROBLEM: {problem}")
    if problems:
        print(f"release-preflight: REFUSED: {plural(len(problems), 'problem')}; nothing was changed")
        return 1
    print(f"release-preflight: OK: HEAD {facts['head'][:12]} is origin/{args.base}, the tree is clean, "
          f"{facts['local_tags']} local tags all on origin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
