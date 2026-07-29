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
own append-only rule. (`extras/` moved to nevr-runtime-plugins on 2026-07-27.)

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
    # N34/N103: deleted 2026-07-28. Still cited by point-in-time records that
    # correctly describe the tree as it was.
    "src/gameserver",
    "src/gameserver/gameserver.cpp",
    # Gitignored, machine-local. Present in the owner's checkout, absent in every
    # fresh clone and worktree — so requiring it made this checker pass ONLY on
    # one machine, which is the exact "green on the author's box" failure that
    # docs/standards/verification.md exists to prevent. Two independent agents hit
    # it in fresh worktrees on 2026-07-27.
    "extern/protobuf",
}


# A historical citation: `git show <sha>:<path>`. The path deliberately does NOT
# exist on disk — that is the point of citing it. Recording a 40-hex sha plus the
# path is what makes deleting a stale document safe, so this checker must not
# demand the file be present.
#
# It does something better instead: it VERIFIES the citation, with
# `git cat-file -e <sha>:<path>`. A citation nobody checked is worse than no
# citation, because it looks like evidence. Typo the sha and you get a
# convincing-looking pointer to nothing.
CITATION_RE = re.compile(r"git show ([0-9a-f]{40}):([A-Za-z0-9_./-]+)")


def citations():
    """(file, sha, path) for every `git show <sha>:<path>` in a tracked .md."""
    files = subprocess.run(["git", "ls-files", "*.md"], cwd=REPO,
                           capture_output=True, text=True).stdout.split()
    for f in files:
        text = (REPO / f).read_text(errors="replace")
        for m in CITATION_RE.finditer(text):
            yield f, m.group(1), m.group(2)


def cited_paths() -> set:
    return {path for _f, _sha, path in citations()}


def claimed_paths():
    files = subprocess.run(["git", "ls-files", "*.md"], cwd=REPO,
                           capture_output=True, text=True).stdout.split()
    for f in files:
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
    # Verify every historical citation actually resolves in git history.
    dead_citations = []
    for f, sha, path in citations():
        rc = subprocess.run(["git", "cat-file", "-e", f"{sha}:{path}"],
                            cwd=REPO, capture_output=True).returncode
        if rc != 0:
            dead_citations.append((f, sha, path))
    for f, sha, path in dead_citations:
        print(f"doc-paths: FAIL {f} cites `git show {sha[:12]}...:{path}` "
              f"but that object does not exist in this repository.", file=sys.stderr)

    cited = cited_paths()

    bad = []
    for f, raw, p in claimed_paths():
        if p in ALLOWED_ABSENT:
            continue
        if p in cited:
            continue      # deliberately deleted and cited; the citation was checked above
        if not (REPO / p).exists():
            bad.append((f, raw))
    for f, raw in bad:
        print(f"doc-paths: FAIL {f} claims `{raw}` — no such path", file=sys.stderr)
    if bad or dead_citations:
        if bad:
            print(f"\ndoc-paths: {len(bad)} claimed path(s) do not exist. A document that "
                  f"names a path that is not there sends the next reader somewhere real "
                  f"and wrong. If the file was deliberately removed, cite it instead: "
                  f"`git show <40-hex-sha>:<path>`.", file=sys.stderr)
        if dead_citations:
            print(f"\ndoc-paths: {len(dead_citations)} historical citation(s) do not "
                  f"resolve. Get the sha with `git log -1 --format=%H -- <path>` and "
                  f"confirm with `git show <sha>:<path>` BEFORE writing it down.",
                  file=sys.stderr)
        return 1
    n = len(cited_paths())
    print(f"doc-paths: OK (all claimed repo paths resolve"
          + (f"; {n} historical citation(s) verified against git history)" if n else ")"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
