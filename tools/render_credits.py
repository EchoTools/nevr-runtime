#!/usr/bin/env python3
"""Render README.md's "Built on, and thanks" block from config/credits.json (#892).

config/credits.json is the one list of the projects and people this project is built on. The README shows each
entry with what it did; the in-game CREDITS page (#938) shows names only, grouped under the headings an
entry is tagged with. This tool is the README half and the one place that says how a heading is chosen for
the in-game half (`ingame_groups`), so the two cannot drift.

    python3 tools/render_credits.py --check   exit 1 and say why when the README block is not the rendering
    python3 tools/render_credits.py --write   regenerate the block between the markers in README.md
    python3 tools/render_credits.py --ingame-source OUT.cpp   the in-game block as a C++ source (the build runs this)
    python3 tools/render_credits.py --ingame-text OUT.txt     the same bytes as a file, for tests and review

File shape (schema 1; every key is required):
  headings  ordered [{id, ingame}]       in-game group titles (the same character rule as a name); a heading
                                         with no entry shows nowhere
  sections  ordered [{id, title}]        README groups
  entries   ordered [{name, section, headings, link, what, sources}]
              name      the display name, spelled as the person or project writes it; each name once. It is
                        drawn in-game too (when a heading lists it), so it is printable ASCII or "\u00a9"
                        (the game's font draws nothing else), at most 64 bytes; any other character is refused
              section   a sections id, or null for a name the README does not show
              headings  heading ids the in-game page lists this name under (may be empty)
              link      "" or https://github.com/<owner>[/<repo>]
              what      one line for the README; required when section is not null
              sources   evidence for `what`: a github.com URL, "#<number>" of this repo, a
                        repo-relative path, "named by the project owner[, MM-DD]" (no chat quoted), or one of the
                        two exact texts "echoApkPatch credit strings" (the community installer's patcher thanks
                        the name; the patcher's repository is private) and "tested in #nevr-runtime-testing
                        (Discord)" (the name tested in that channel)
  notes     ordered [text], each non-empty: rules for whoever edits this file (placement of a name, where a
            source lives). Never rendered anywhere; this file is where they are kept so they travel with the data
  closing   one README line after the lists, or ""

Every top-level and entry key is required and an unknown one is refused, so a mistyped key cannot silently
drop data.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_JSON = ROOT / "config" / "credits.json"
DEFAULT_README = ROOT / "README.md"

BEGIN = "<!-- credits:begin (generated from config/credits.json by tools/render_credits.py; edit the JSON, then run it with --write) -->"
END = "<!-- credits:end -->"
BLOCK = re.compile(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n", re.S)

# A link is a GitHub profile or a repository, nothing that hands out builds.
LINK_SHAPE = re.compile(r"^https://github\.com/[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)?$")
URL = re.compile(r"https?://[^\s)>\]]+")
SOURCE_URL = re.compile(r"^https://github\.com/[A-Za-z0-9_.-]+(/[A-Za-z0-9_.#/-]+)?$")
SOURCE_ISSUE = re.compile(r"^#[0-9]+$")
# The project owner named the person or the work; the record of it is not public, so it is stated as that and nothing more.
SOURCE_OWNER = re.compile(r"^named by the project owner(, [0-9]{2}-[0-9]{2})?$")
# Two records that are not public by URL: the patcher's repository is private, and the testing channel is a chat.
# Each is one exact text, so a free-form "source" cannot come in through them.
SOURCE_TEXTS = frozenset({"echoApkPatch credit strings", "tested in #nevr-runtime-testing (Discord)"})
# The in-game page (#938). The game shows a line of at most 64 bytes of single-byte (Windows-1252) text, one
# actor per LF-terminated line, and splits a line at '|' into two columns, so a name carries none of those.
INGAME_ENCODING = "cp1252"
INGAME_MAX_LINE_BYTES = 64
INGAME_REVIEW_LINE_BYTES = 40
INGAME_MAX_LINES = 200
INGAME_BLANKS_AFTER_GROUP = 3
INGAME_FORBIDDEN = set("\n\r\x00|")
# What the game's font draws, measured on the running game (#938): printable ASCII and the copyright sign (0xA9,
# which the game's own credits use). U+00EB (0xEB) draws as '?', so a character outside this set is refused at
# build time, never replaced; a further character is added only after it has been seen drawn.
INGAME_DRAWN = frozenset(chr(c) for c in range(0x20, 0x7F)) | {"\u00a9"}
KEYS = {"schema", "headings", "sections", "entries", "notes", "closing"}
ENTRY_KEYS = {"name", "section", "headings", "link", "what", "sources"}


def validate(data: dict, root: Path = ROOT) -> list[str]:
    problems: list[str] = []
    if not isinstance(data, dict) or set(data) != KEYS:
        return [f"top-level keys must be exactly {sorted(KEYS)}"]
    if data["schema"] != 1:
        problems.append("schema must be 1")
    notes = data["notes"]
    if not isinstance(notes, list) or not all(isinstance(n, str) and n.strip() for n in notes):
        problems.append("notes must be a list of non-empty texts")
    heading_ids = [h.get("id") for h in data["headings"]]
    section_ids = [s.get("id") for s in data["sections"]]
    for kind, ids in (("heading", heading_ids), ("section", section_ids)):
        if len(ids) != len(set(ids)) or not all(isinstance(i, str) and i for i in ids):
            problems.append(f"{kind} ids must be unique non-empty strings")
    for heading in data["headings"]:
        if not heading.get("ingame"):
            problems.append(f"heading {heading.get('id')!r} has no in-game text")
    for section in data["sections"]:
        if not section.get("title"):
            problems.append(f"section {section.get('id')!r} has no title")
    seen: set[str] = set()
    for entry in data["entries"]:
        name = entry.get("name")
        if set(entry) != ENTRY_KEYS:
            problems.append(f"{name!r}: keys must be exactly {sorted(ENTRY_KEYS)}")
            continue
        if not isinstance(name, str) or not name:
            problems.append("an entry has no name")
            continue
        if name in seen:
            problems.append(f"{name}: appears more than once")
        seen.add(name)
        if entry["section"] is not None and entry["section"] not in section_ids:
            problems.append(f"{name}: unknown section {entry['section']!r}")
        for tag in entry["headings"]:
            if tag not in heading_ids:
                problems.append(f"{name}: unknown heading {tag!r}")
        if len(entry["headings"]) != len(set(entry["headings"])):
            problems.append(f"{name}: a heading is listed twice")
        if entry["link"] and not LINK_SHAPE.match(entry["link"]):
            problems.append(f"{name}: link {entry['link']!r} is not github.com/<owner>[/<repo>]")
        if entry["section"] is not None and not entry["what"].strip():
            problems.append(f"{name}: shown in the README with nothing said")
        for url in URL.findall(entry["what"]):
            if not LINK_SHAPE.match(url):
                problems.append(f"{name}: link {url!r} in the text is not github.com/<owner>[/<repo>]")
        if not entry["sources"]:
            problems.append(f"{name}: no source")
        for source in entry["sources"]:
            ok = bool(SOURCE_URL.match(source) or SOURCE_ISSUE.match(source) or SOURCE_OWNER.match(source) or source in SOURCE_TEXTS or (
                not source.startswith(("http", "/")) and ".." not in source and (root / source).exists()))
            if not ok:
                problems.append(f"{name}: source {source!r} is not a github.com URL, an issue number, 'named by the project owner[, MM-DD]', one of {sorted(SOURCE_TEXTS)} or a repo path that exists")
    for url in URL.findall(data["closing"]):
        problems.append(f"closing: link {url!r} not allowed")
    if not problems:
        problems += ingame_problems(data)
    return problems


def render_readme(data: dict) -> str:
    """The README block, markers included, for every entry that has a section."""
    lines = [BEGIN, ""]
    for section in data["sections"]:
        entries = [e for e in data["entries"] if e["section"] == section["id"]]
        if not entries:
            continue
        lines += [f"### {section['title']}", ""]
        for entry in entries:
            who = f"[{entry['name']}]({entry['link']})" if entry["link"] else entry["name"]
            lines.append(f"- **{who}**: {entry['what']}")
        lines.append("")
    if data["closing"]:
        lines += [data["closing"], ""]
    lines.append(END)
    return "\n".join(lines) + "\n"


def ingame_groups(data: dict) -> list[dict]:
    """The in-game page: one group per heading that has a name, in heading order, names in file order.
    A heading with no name is left out, so an empty heading shows nowhere."""
    groups = []
    for heading in data["headings"]:
        names = [e["name"] for e in data["entries"] if heading["id"] in e["headings"]]
        if names:
            groups.append({"id": heading["id"], "ingame": heading["ingame"], "names": names})
    return groups


def _ingame_lines(data: dict) -> list[str]:
    """Each shown group: its heading, a blank line, its names, then blank lines before the next group."""
    lines: list[str] = []
    for group in ingame_groups(data):
        lines += [group["ingame"], ""] + group["names"] + [""] * INGAME_BLANKS_AFTER_GROUP
    return lines


def _ingame_texts(data: dict) -> list[tuple[str, str]]:
    """(what it is, the text) for every heading and every name shown in-game."""
    texts = [(f"heading {h['id']!r}, field ingame", h["ingame"]) for h in data["headings"]]
    for group in ingame_groups(data):
        texts += [(f"entry {name!r}, field name", name) for name in group["names"]]
    return texts


def ingame_text(data: dict) -> bytes:
    """The bytes the runtime puts above the game's own credits: LF-terminated lines in the game's encoding.
    Empty when no heading has a name. A character the game's font cannot draw raises ValueError here, so a
    caller that skipped validate() still gets no bytes for it."""
    for what, text in _ingame_texts(data):
        undrawn = _undrawn(text)
        if undrawn:
            raise ValueError(f"{what}: {undrawn} is not drawn by the game's font")
    return "".join(line + "\n" for line in _ingame_lines(data)).encode(INGAME_ENCODING)


def _undrawn(text: str) -> str:
    """The characters of text the game's font cannot draw, each as repr and code point, empty when there are none."""
    return ", ".join(f"{c!r} (U+{ord(c):04X})" for c in sorted({c for c in text if c not in INGAME_DRAWN}))


def ingame_problems(data: dict) -> list[str]:
    """Why the in-game page cannot show this file, one string each. Refused here, at build time, rather than
    cut or dropped by the game or the runtime."""
    problems: list[str] = []
    for what, text in _ingame_texts(data):
        if not text or text != text.strip(" "):
            problems.append(f"{what}: empty, or starts or ends with a space")
            continue
        bad = sorted({c for c in text if c in INGAME_FORBIDDEN or ord(c) < 0x20})
        if bad:
            problems.append(f"{what}: has a character the in-game page parses ({', '.join(repr(c) for c in bad)})")
            continue
        undrawn = _undrawn(text)
        if undrawn:
            problems.append(f"{what}: {undrawn} is not drawn by the game's font (printable ASCII and the copyright sign "
                            "are); refused, not replaced")
            continue
        size = len(text.encode(INGAME_ENCODING))
        if size > INGAME_MAX_LINE_BYTES:
            problems.append(f"{what}: {size} bytes, the game shows at most {INGAME_MAX_LINE_BYTES}")
    lines = len(_ingame_lines(data))
    if lines > INGAME_MAX_LINES:
        problems.append(f"the in-game block is {lines} lines, the limit is {INGAME_MAX_LINES}")
    return problems


def ingame_warnings(data: dict) -> list[str]:
    """Texts a reviewer should look at on screen: valid, but over the review length."""
    warnings = []
    for what, text in _ingame_texts(data):
        try:
            size = len(text.encode(INGAME_ENCODING))
        except UnicodeEncodeError:
            continue
        if INGAME_REVIEW_LINE_BYTES < size <= INGAME_MAX_LINE_BYTES:
            warnings.append(f"{what}: {size} bytes, over the {INGAME_REVIEW_LINE_BYTES} a reviewer checks on screen")
    return warnings


def _c_piece(line: bytes) -> str:
    """One C string literal for one line. Octal escapes are fixed at three digits, so a digit that follows
    cannot extend one (a hex escape would read '\\xa9' + '9' as one character)."""
    out = []
    for b in line:
        c = chr(b)
        if 0x20 <= b < 0x7F and c not in '"\\?':
            out.append(c)
        elif c in '"\\':
            out.append("\\" + c)
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def ingame_source(data: dict) -> str:
    """The block as a C++ source: kText (NUL-terminated) and kSize (its length without the NUL)."""
    block = ingame_text(data)
    lines = block.splitlines(keepends=True)
    literal = "\n    ".join(_c_piece(line) for line in lines) if lines else '""'
    return (
        "// Generated by tools/render_credits.py --ingame-source from config/credits.json. Do not edit.\n"
        '#include "runtime/patch/credits_block.h"\n\n'
        "namespace nevr_credits_block {\n\n"
        f"const char kText[] =\n    {literal};\n"
        f"const std::size_t kSize = {len(block)};\n\n"
        "}  // namespace nevr_credits_block\n"
    )


def check(json_path: Path, readme_path: Path) -> list[str]:
    data = json.loads(Path(json_path).read_text(encoding="utf-8"))
    problems = validate(data, ROOT)
    if problems:
        return problems
    text = Path(readme_path).read_text(encoding="utf-8")
    blocks = BLOCK.findall(text)
    if len(blocks) != 1 or text.count(BEGIN) != 1 or text.count(END) != 1:
        return [f"{readme_path}: expected exactly one credits block between the markers, found {len(blocks)}"]
    if blocks[0] != render_readme(data):
        return [f"{readme_path}: the credits block is not what {json_path} renders; run tools/render_credits.py --write"]
    return []


def write(json_path: Path, readme_path: Path) -> list[str]:
    data = json.loads(Path(json_path).read_text(encoding="utf-8"))
    problems = validate(data, ROOT)
    if problems:
        return problems
    text = Path(readme_path).read_text(encoding="utf-8")
    if len(BLOCK.findall(text)) != 1:
        return [f"{readme_path}: no credits block between the markers to replace"]
    block = render_readme(data)
    Path(readme_path).write_text(BLOCK.sub(lambda _m: block, text, count=1), encoding="utf-8")
    return []


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="fail unless the README block is the rendering")
    mode.add_argument("--write", action="store_true", help="regenerate the README block")
    mode.add_argument("--ingame-source", type=Path, metavar="OUT", help="write the in-game block as a C++ source")
    mode.add_argument("--ingame-text", type=Path, metavar="OUT", help="write the in-game block's bytes")
    parser.add_argument("--json", type=Path, default=DEFAULT_JSON)
    parser.add_argument("--readme", type=Path, default=DEFAULT_README)
    args = parser.parse_args(argv)
    if args.ingame_source or args.ingame_text:
        data = json.loads(Path(args.json).read_text(encoding="utf-8"))
        problems = validate(data, ROOT)
        for problem in problems:
            print(f"render_credits: {problem}", file=sys.stderr)
        if problems:
            return 1
        for warning in ingame_warnings(data):
            print(f"render_credits: warning: {warning}", file=sys.stderr)
        if args.ingame_source:
            args.ingame_source.parent.mkdir(parents=True, exist_ok=True)
            args.ingame_source.write_text(ingame_source(data), encoding="ascii", newline="\n")
            print(f"render_credits: wrote {args.ingame_source}")
        else:
            args.ingame_text.parent.mkdir(parents=True, exist_ok=True)
            args.ingame_text.write_bytes(ingame_text(data))
            print(f"render_credits: wrote {args.ingame_text}")
        return 0
    problems = check(args.json, args.readme) if args.check else write(args.json, args.readme)
    for problem in problems:
        print(f"render_credits: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"render_credits: {'ok' if args.check else 'wrote'} {args.readme}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
