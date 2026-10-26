#!/usr/bin/env python3
"""Render README.md's "Built on, and thanks" block from config/credits.json (#892).

config/credits.json is the one list of the projects and people this project is built on. The README shows each
entry with what it did; the in-game CREDITS page (#938) shows names only, grouped under the headings an
entry is tagged with. This tool is the README half and the one place that says how a heading is chosen for
the in-game half (`ingame_groups`), so the two cannot drift.

    python3 tools/render_credits.py --check   exit 1 and say why when the README block is not the rendering
    python3 tools/render_credits.py --write   regenerate the block between the markers in README.md

File shape (schema 1; every key is required):
  headings  ordered [{id, ingame}]       in-game group titles; a heading with no entry shows nowhere
  sections  ordered [{id, title}]        README groups
  entries   ordered [{name, section, headings, link, what, sources}]
              name      the display name, spelled as the person or project writes it; each name once
              section   a sections id, or null for a name the README does not show
              headings  heading ids the in-game page lists this name under (may be empty)
              link      "" or https://github.com/<owner>[/<repo>]
              what      one line for the README; required when section is not null
              sources   public evidence for `what`: a github.com URL, "#<number>" of this repo, or a
                        repo-relative path
  closing   one README line after the lists, or ""
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
KEYS = {"schema", "headings", "sections", "entries", "closing"}
ENTRY_KEYS = {"name", "section", "headings", "link", "what", "sources"}


def validate(data: dict, root: Path = ROOT) -> list[str]:
    problems: list[str] = []
    if not isinstance(data, dict) or set(data) != KEYS:
        return [f"top-level keys must be exactly {sorted(KEYS)}"]
    if data["schema"] != 1:
        problems.append("schema must be 1")
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
            ok = bool(SOURCE_URL.match(source) or SOURCE_ISSUE.match(source) or (
                not source.startswith(("http", "/")) and ".." not in source and (root / source).exists()))
            if not ok:
                problems.append(f"{name}: source {source!r} is not a github.com URL, an issue number or a repo path that exists")
    for url in URL.findall(data["closing"]):
        problems.append(f"closing: link {url!r} not allowed")
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
    parser.add_argument("--json", type=Path, default=DEFAULT_JSON)
    parser.add_argument("--readme", type=Path, default=DEFAULT_README)
    args = parser.parse_args(argv)
    problems = check(args.json, args.readme) if args.check else write(args.json, args.readme)
    for problem in problems:
        print(f"render_credits: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"render_credits: {'ok' if args.check else 'wrote'} {args.readme}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
