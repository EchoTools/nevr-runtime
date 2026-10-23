#!/usr/bin/env python3
"""Check that a PR body names at least one issue the PR resolves or advances.

Every PR is opened for an existing issue: the issue states the problem and its done-bar, and the PR
names it (docs/standards/planning.md, "PRs"). There is no "no issue" form.

A reference is a line that starts with a keyword and then an issue number:

    Closes #N | Fixes #N | Resolves #N     the PR resolves the issue (GitHub closes it on merge)
    Refs #N | Part of #N | Completes #N    the PR advances it; the body says what is still open

`owner/repo#N` names an issue in another repository. A reference inside an HTML comment, a code span
or a fenced block does not count, so the PR template's placeholder and its instructions never satisfy
the check. The check cannot tell whether the diff really resolves the issue; the review does.

Standard library only.

    PR_BODY=... check_pr_issue_link.py
    check_pr_issue_link.py --body-file body.txt

Exit 0 and one `ok:` line on success; exit 1 and one `FAIL:` line on stderr otherwise.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

CLOSING = ("close", "closes", "closed", "fix", "fixes", "fixed", "resolve", "resolves", "resolved")
PARTIAL = ("refs", "ref", "part of", "completes")

REFERENCE = re.compile(
    r"^[ \t]*(?:[-*][ \t]+)?(?P<word>" + "|".join(sorted(CLOSING + PARTIAL, key=len, reverse=True)) + r")"
    r"[ \t]*:?[ \t]+(?P<repo>[\w.-]+/[\w.-]+)?#(?P<number>[1-9][0-9]*)\b",
    re.IGNORECASE | re.MULTILINE,
)
HTML_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
FENCE = re.compile(r"^[ \t]*(```|~~~).*?^[ \t]*\1[ \t]*$", re.DOTALL | re.MULTILINE)
CODE_SPAN = re.compile(r"`[^`\n]*`")


class Failure(Exception):
    pass


def references(body: str) -> list[str]:
    """Return the issue references the body carries, as `Word #N` strings, in order."""
    text = body.replace("\r\n", "\n").replace("\r", "\n")
    text = HTML_COMMENT.sub("", text)
    text = FENCE.sub("", text)
    text = CODE_SPAN.sub("", text)
    return [f"{m.group('word')} {m.group('repo') or ''}#{m.group('number')}" for m in REFERENCE.finditer(text)]


def check(body: str) -> list[str]:
    found = references(body)
    if not found:
        raise Failure("the PR body names no issue; open the issue first, then add a line `Closes #N` for each "
                      "issue it resolves, or `Refs #N` for one it only advances (and say what is still open)")
    return found


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--body-file", help="file holding the PR body; default: the PR_BODY environment variable")
    args = parser.parse_args(argv)
    try:
        if args.body_file:
            with open(args.body_file, encoding="utf-8", newline="") as handle:
                body = handle.read()
        else:
            body = os.environ.get("PR_BODY", "")
        found = check(body)
    except (Failure, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("ok: the PR body names " + ", ".join(found))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
