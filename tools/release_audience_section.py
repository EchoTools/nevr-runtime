#!/usr/bin/env python3
"""The per-audience section of a release notes, from the feat, fix and perf commits between two refs (RELEASING.md,
"Write the release body"). Usage: release_audience_section.py --repo <dir> --base vX.Y.Z --rev vX.Y.Z

The release body is short and carries only its player line, `player_line()`, which `package_release.py notes` calls
over the same commits. The line is built only from feat commits whose audience is players; this script's section is for a
reader of the commits, not for the body. A `release-headline.txt` at the repository root, changed in base..rev, replaces
that line in the body (`package_release.headline_line`); this script does not read it.

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
# The issue or PR references that end a subject: "(#1340)", "(refs #1220) (#1340)", "(closes #1285)". Only at the end,
# only a parenthesis holding `#<digits>` with an optional listed word first.
ISSUE_REFERENCE = re.compile(r"(?:\s*\((?:(?:refs|closes|fixes|resolves|completes|part of)\s+)?#\d+(?:\s*,\s*#\d+)*\))+\s*$", re.I)

NOT_TESTED = (
    "Each pull request ran the quick check its body names: the build and the tests that cover the changed code. "
    "`just verify` did not run for this tag, and the tag's build skips it (#1008): the build and packaging are what the "
    "tag proves."
)


FALLBACK_PLAYER_LINE = "Fixes and internal changes."


def audience_of(scope: str | None) -> str:
    return SCOPE_AUDIENCE.get(scope or "", "For players")


def messages_between(repo: str, base: str, rev: str) -> list[str]:
    """The full messages of the non-merge commits in base..rev, newest first as git prints them."""
    log = subprocess.run(
        ["git", "-C", repo, "log", "--no-merges", f"--format=%B{RECORD_END}", f"{base}..{rev}"],
        check=True, capture_output=True, text=True,
    ).stdout
    return [record.strip() for record in log.split(RECORD_END) if record.strip()]


def listed(messages: list[str]) -> list[tuple[str, str, str]]:
    """Each listed change as (audience, type, text), in the order given. The subject gives the type, scope and `!`, and
    a `BREAKING CHANGE:` footer marks a break too; the text is the message's `Release-note:` line when it has one, else
    the subject's text, and a breaking change's text starts with "BREAKING: ". The one place a commit is classified."""
    out = []
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
        out.append((audience_of(scope), match.group("type").lower(), f"{prefix}{text}"))
    return out


def classify(messages: list[str]) -> dict[str, list[str]]:
    """Each listed change's text, under its audience."""
    by_audience: dict[str, list[str]] = {name: [] for name in AUDIENCES}
    for audience, _kind, text in listed(messages):
        by_audience[audience].append(text)
    return by_audience


def player_line(messages: list[str]) -> str:
    """The one line of the release body for players, built only from feat commits whose audience is players: the first
    two such texts joined by "; ", then "and N more" for the rest of the player feats, ended by a period. Each shown text
    loses its trailing issue or PR references (ISSUE_REFERENCE), and the first letter of the line is capitalised. A fix,
    perf or any other type never contributes; with no player feat the line is FALLBACK_PLAYER_LINE."""
    texts = [text for audience, kind, text in listed(messages) if audience == "For players" and kind == "feat"]
    if not texts:
        return FALLBACK_PLAYER_LINE
    line = "; ".join(ISSUE_REFERENCE.sub("", text).strip() for text in texts[:2])
    line = line[:1].upper() + line[1:]
    if len(texts) > 2:
        line += f" and {len(texts) - 2} more"
    return line.rstrip(".") + "."


def sections(messages: list[str]) -> str:
    """The markdown section for the given commit messages (a subject, or a subject, a blank line and a body), newest
    first as git prints them."""
    by_audience = classify(messages)
    out = ["## What changed, by audience", ""]
    for name in AUDIENCES:
        lines = [f"- {text}" for text in by_audience[name]] or ["- Nothing for this audience in this release."]
        out.extend([f"### {name}", "", *lines, ""])
    out.extend(["## Not tested in this build", "", NOT_TESTED, ""])
    return "\n".join(out)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=".", help="the repository (default: the current directory)")
    parser.add_argument("--base", required=True, help="the older ref, excluded")
    parser.add_argument("--rev", required=True, help="the newer ref, included")
    args = parser.parse_args(argv)
    sys.stdout.write(sections(messages_between(args.repo, args.base, args.rev)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
