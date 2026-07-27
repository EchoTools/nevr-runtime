#!/usr/bin/env python3
"""Every repo path claimed in a document shall resolve.

Facts about the tree — layouts, source paths, plugin rosters — were asserted in
prose in three documents and drifted apart silently. README.md and CLAUDE.md
carried four paths that did not exist (`src/protobufnevr/`, `extern/nevr-proto`,
`src/server/`, `gamepatches/patches.cpp`) for months, because prose cannot be
wrong in a way anything notices.

Scope: CURRENT-STATE documents only. A record is immutable — `BUGS.md` entries and
`docs/audits/` describe the tree as it was when written, and a path that has since
moved is correct history, not a defect. Amending them would violate the ledger's
own append-only rule. `extras/` is quarantine, reviewed by the owner separately.

Checked: README.md, AGENTS.md, CLAUDE.md, docs/ (except audits), tests/**/README.md.

Backticked paths in those files that look like repo paths. A path is
"claimed" if it starts with a known top-level directory. Trailing slashes and
line-suffixes (`file.cpp:123`) are stripped before the check.

Exit 0 = every claimed path resolves. Exit 1 = at least one does not.
"""
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOTS = ("src/", "plugins/", "tools/", "docs/", "extern/", "gen/", "tests/", "cmake/")

# Paths that are deliberately referenced but shall not exist on disk.
ALLOWED_ABSENT = {
    # N34: the dead copy is cited precisely so agents avoid it.
    "src/gameserver",
    "src/gameserver/gameserver.cpp",
    # Gitignored, machine-local. Present in the owner's checkout, absent in every
    # fresh clone and worktree — so requiring it made this checker pass ONLY on
    # one machine, which is the exact "green on the author's box" failure that
    # docs/standards/verification.md exists to prevent. Two independent agents hit
    # it in fresh worktrees on 2026-07-27.
    "extern/protobuf",
}


def claimed_paths():
    files = subprocess.run(["git", "ls-files", "*.md"], cwd=REPO,
                           capture_output=True, text=True).stdout.split()
    for f in files:
        if f.startswith("extras/"):          # owner-managed quarantine
            continue
        if f == "BUGS.md" or f.startswith("docs/audits/"):
            continue                          # immutable records — see module docstring
        text = (REPO / f).read_text(errors="replace")
        for m in re.finditer(r"`([A-Za-z0-9_./-]+)`", text):
            raw = m.group(1)
            if not raw.startswith(ROOTS):
                continue
            # strip :line / :line-line suffixes and trailing slash
            p = re.sub(r":\d+(-\d+)?$", "", raw).rstrip("/")
            # skip globs and obvious prose
            if "*" in p or p.endswith("."):
                continue
            yield f, raw, p


def main() -> int:
    bad = []
    for f, raw, p in claimed_paths():
        if p in ALLOWED_ABSENT:
            continue
        if not (REPO / p).exists():
            bad.append((f, raw))
    for f, raw in bad:
        print(f"doc-paths: FAIL {f} claims `{raw}` — no such path", file=sys.stderr)
    if bad:
        print(f"\ndoc-paths: {len(bad)} claimed path(s) do not exist. A document that "
              f"names a path that is not there sends the next reader somewhere real "
              f"and wrong.", file=sys.stderr)
        return 1
    print("doc-paths: OK (all claimed repo paths resolve)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
