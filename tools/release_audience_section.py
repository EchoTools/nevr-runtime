#!/usr/bin/env python3
"""The per-audience section of a release body, from the feat, fix and perf commits between two refs (RELEASING.md,
"Write the release body"). Usage: release_audience_section.py --repo <dir> --base vX.Y.Z --rev vX.Y.Z

A commit's audience is its conventional scope: `plugins` is for plugin developers, `server`, `ws-bridge` and `arena` for
server hosts, and every other scope, or none, is for players. A commit is listed by its subject, or by the text of a
`Release-note:` line in its message when it has one (a subject says what the change did to the code, and can overstate or
misstate what a player or host sees). A breaking change (`!` in the subject, or a `BREAKING CHANGE:` or `BREAKING-CHANGE:`
footer) is listed under its audience with "BREAKING:" in front. Types and scopes match in any case, as Conventional
Commits allows. The section ends with what was not tested in the build, which is the same for every release.
"""

import argparse
import re
import subprocess
import sys

AUDIENCES = ("For players", "For server hosts", "For plugin developers")
SCOPE_AUDIENCE = {
    "server": "For server hosts",
    "ws-bridge": "For server hosts",
    "arena": "For server hosts",
    "plugins": "For plugin developers",
    "plugin": "For plugin developers",
}
# Tooling, CI and the release process itself change no shipped file, so no audience reads them: they are left out.
EXCLUDED_SCOPES = {"tools", "ci", "build", "test", "docs", "release"}
CONVENTIONAL = re.compile(r"^(?P<type>feat|fix|perf)(?:\((?P<scope>[^)]*)\))?(?P<bang>!)?: (?P<text>.+)$", re.I)
BREAKING_FOOTER = re.compile(r"^BREAKING[ -]CHANGE:[ \t]", re.M)
RELEASE_NOTE = re.compile(r"^Release-note:[ \t]*(?P<text>\S.*?)[ \t]*$", re.M)
RECORD_END = "\x1e"

NOT_TESTED = (
    "Each pull request ran the quick check its body names: the build and the tests that cover the changed code. "
    "`just verify` did not run for this tag, and the tag's build skips it (#1008): the build and packaging are what the "
    "tag proves."
)


def audience_of(scope: str | None) -> str:
    return SCOPE_AUDIENCE.get(scope or "", "For players")


def sections(messages: list[str]) -> str:
    """The markdown section for the given commit messages (a subject, or a subject, a blank line and a body), newest
    first as git prints them. The subject gives the type, scope and `!`, and a `BREAKING CHANGE:` footer marks a break
    too; the text listed is the message's `Release-note:` line when it has one, else the subject's text."""
    by_audience: dict[str, list[str]] = {name: [] for name in AUDIENCES}
    for message in messages:
        subject = message.split("\n", 1)[0]
        match = CONVENTIONAL.match(subject)
        if match is None:
            continue
        scope = match.group("scope").lower() if match.group("scope") is not None else None
        if scope in EXCLUDED_SCOPES:
            continue
        note = RELEASE_NOTE.search(message)
        text = note.group("text") if note else match.group("text")
        body = message.split("\n", 1)[1] if "\n" in message else ""
        breaking = match.group("bang") or BREAKING_FOOTER.search(body)
        prefix = "BREAKING: " if breaking else ""
        by_audience[audience_of(scope)].append(f"- {prefix}{text}")
    out = ["## What changed, by audience", ""]
    for name in AUDIENCES:
        out.append(f"### {name}")
        out.append("")
        out.extend(by_audience[name] or ["- Nothing for this audience in this release."])
        out.append("")
    out.extend(["## Not tested in this build", "", NOT_TESTED, ""])
    return "\n".join(out)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=".", help="the repository (default: the current directory)")
    parser.add_argument("--base", required=True, help="the older ref, excluded")
    parser.add_argument("--rev", required=True, help="the newer ref, included")
    args = parser.parse_args(argv)
    log = subprocess.run(
        ["git", "-C", args.repo, "log", "--no-merges", f"--format=%B{RECORD_END}", f"{args.base}..{args.rev}"],
        check=True, capture_output=True, text=True,
    ).stdout
    sys.stdout.write(sections([record.strip() for record in log.split(RECORD_END) if record.strip()]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
