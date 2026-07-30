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
    # EXCISED by board ruling (RULINGS.md 2026-07-20 "Test harness excised"). The
    # engineer primer names it precisely to say it is gone and must not be
    # re-added; the real harness lives outside the repo and is not wired in.
    "extern/evr-test-harness",
    # Pre-rename layer/directory names. The engineer primer cites these to record
    # WHAT MOVED WHERE (N108 split `src/common/`; N109 renamed `src/gamepatches/`;
    # N105 deleted `src/modules/ws-bridge/`). A rename note has to name the old
    # path or it cannot do its job — that is the opposite of a stale claim.
    "src/gamepatches",
    "src/common",
    "src/modules/ws-bridge",
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

# A backticked BARE filename — `foo.cpp`, not `src/runtime/foo.cpp`. The
# directory-prefixed check below cannot see these, because it only inspects
# tokens starting with a known ROOT. That blind spot let two renames rot in
# place: `wave0_instrumentation.cpp` (renamed to binary_bug_fixes.cpp in the
# 2026-07-29 reorganisation) and `builtin_server_timing.cpp` (deleted as dead
# code, ledger N26). Both read as current source files and neither existed.
#
# A bare filename is resolved against every tracked BASENAME in the repo, so it
# does not care which directory the file lives in — which is exactly right: a
# rename that only moves a file should not fail, and a rename that changes the
# NAME should.
BARE_FILE_RE = re.compile(r"`([A-Za-z0-9_][A-Za-z0-9_.\-]*\.(?:cpp|h|hpp|py|def|cmake|sh))`")

# Bare filenames that legitimately do not resolve here. Each needs a reason —
# this list is how the check stays useful rather than becoming a place to dump
# anything inconvenient.
BARE_ALLOWED = {
    # Naming-convention EXAMPLES, not files. Deliberately not real.
    "SHOUTY.md", "lowercase-kebab-case.md", "2026-07-26-thing.md",
    # Pre-rename name, cited by the engineer primer to record the rename itself
    # (N109 moved AND renamed it to src/runtime/log/builtin_filter.cpp). Naming
    # the old file is the whole point of a rename note.
    "builtin_log_filter.cpp",
    # Files owned by other projects / toolchains, correctly named and not ours.
    "android.toolchain.cmake",          # Android NDK
    "CPP-MINGW-ADDENDUM-GENERIC.md",    # ~/src/metis-core, a mandatory pre-read
    "CNSRADFriends_protocol.h",         # echovr-reconstruction
    "pnsrad.def",                       # echovr-reconstruction
    "dxvk-interop.h",                   # DXVK, referenced as a porting option
    # A file a design doc PROPOSES and which was deliberately never built.
    # See the status header on 2026-06-09-streaming-integration.md.
    "stream_client.cpp",
}


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


# Machine-local agent/skill definitions. These are GITIGNORED (`.gitignore:145`
# ignores `.claude/`) and deliberately stay that way: the engineer primer names
# the private hardening overlay, its local symlink path, and the purged leak
# commit, none of which may be republished into this public repo.
#
# But "untracked" is exactly why the primer rotted. It sat outside every gate and
# accumulated 14 references to `src/gamepatches/`, a directory deleted in N109,
# plus a `src/common/` layer split apart in N108 — and a dispatched agent walked
# into all of them before anything noticed. A file does not have to be committed
# to be checked; it only has to be on disk.
#
# So these are scanned when present and silently skipped when absent, which keeps
# a fresh clone green while still gating the owner's checkout where the file lives.
LOCAL_DOC_GLOBS = (".claude/agents/*.md", ".claude/skills/*/*.md")


def local_docs():
    for pattern in LOCAL_DOC_GLOBS:
        for p in sorted(REPO.glob(pattern)):
            yield str(p.relative_to(REPO))


def all_doc_files():
    """Tracked current-state markdown, plus machine-local agent/skill definitions."""
    tracked = subprocess.run(["git", "ls-files", "*.md"], cwd=REPO,
                             capture_output=True, text=True).stdout.split()
    seen = set()
    for f in list(tracked) + list(local_docs()):
        if f == "BUGS.md" or f.startswith("docs/audits/"):
            continue                          # immutable records — see module docstring
        if f not in seen:
            seen.add(f)
            yield f


# `path.cpp:123` — a line-number citation. The path half is checked elsewhere;
# this catches the half that drifts independently and far more often. Renaming a
# directory is loud, but a citation that still points at a real file and the
# WRONG line inside it reads as correct right up until someone follows it.
LINE_CITE_RE = re.compile(
    r"`([A-Za-z0-9_./-]+\.(?:cpp|h|hpp|txt|py|def|sh|md|json|cmake)):(\d+)(?:-\d+)?`")


def line_citations():
    for f in all_doc_files():
        for m in LINE_CITE_RE.finditer((REPO / f).read_text(errors="replace")):
            yield f, m.group(1), int(m.group(2))


def claimed_paths():
    for f in all_doc_files():
        text = (REPO / f).read_text(errors="replace")
        # The class MUST include ':'. It did not until 2026-07-30, so a backticked
        # token carrying a line number — `src/runtime/foo.cpp:35`, the single most
        # common way this repo cites code — never matched, and the `re.sub` strip
        # below was dead code that could not fire. Every path:line claim in every
        # document went unchecked for the whole life of this tool. That is how 14
        # `src/gamepatches/*.cpp:NN` references survived the N109 rename: invisible
        # here, and gitignored so the file was not scanned at all.
        for m in re.finditer(r"`([A-Za-z0-9_./:-]+)`", text):
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

    # Bare backticked filenames, resolved by basename against the tracked tree.
    tracked = subprocess.run(["git", "ls-files"], cwd=REPO,
                             capture_output=True, text=True).stdout.split()
    basenames = {pathlib.Path(t).name for t in tracked}
    bare_bad = []
    for f in all_doc_files():
        for m in BARE_FILE_RE.finditer((REPO / f).read_text(errors="replace")):
            name = m.group(1)
            if name in BARE_ALLOWED or name in basenames:
                continue
            bare_bad.append((f, name))
    for f, name in bare_bad:
        print(f"doc-paths: FAIL {f} names `{name}` — no file with that basename "
              f"exists. If it was renamed, use the new name; if removed, drop the "
              f"backticks or cite `git show <sha>:<path>`.", file=sys.stderr)

    # Line-number citations that overrun the file they name.
    stale_lines = []
    for f, path, line in line_citations():
        target = REPO / path
        if not target.exists():
            continue          # the path-existence checks below own this case
        total = len(target.read_text(errors="replace").splitlines())
        if line > total:
            stale_lines.append((f, path, line, total))
    for f, path, line, total in stale_lines:
        print(f"doc-paths: FAIL {f} cites `{path}:{line}` but that file has only "
              f"{total} lines.", file=sys.stderr)

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
    # Each explanation prints for its OWN failure class. This used to be one
    # `if bad or dead_citations or bare_bad:` that returned 1 from inside, which
    # made the bare-filename explanation below unreachable whenever bare_bad was
    # the only failure — the check still failed the build, but with no reason
    # attached, which is the least useful way to fail.
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
    if bare_bad:
        print(f"\ndoc-paths: {len(bare_bad)} bare filename(s) name nothing in the tree. "
              f"These are invisible to the path check above, which only inspects tokens "
              f"starting with a known root — that blind spot is how two renames rotted "
              f"in place.", file=sys.stderr)
    if stale_lines:
        print(f"\ndoc-paths: {len(stale_lines)} line citation(s) overrun their file. A "
              f"path that still resolves while its line number does not is the quietest "
              f"kind of rot: it reads as correct until someone follows it. Re-derive the "
              f"line with grep rather than adjusting it by hand.", file=sys.stderr)
    if bad or dead_citations or bare_bad or stale_lines:
        return 1
    n = len(cited_paths())
    local = sum(1 for _ in local_docs())
    print(f"doc-paths: OK (all claimed repo paths resolve"
          + (f"; {n} historical citation(s) verified against git history" if n else "")
          + (f"; {local} machine-local agent/skill doc(s) scanned" if local else "")
          + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
