#!/usr/bin/env python3
"""Extract the original-binary vulnerability audit from BUGS.md into a
ReVault-ingestible form.

Why this exists: BUGS.md is a high-frequency agent scratchpad and is slated to be
removed from this PUBLIC repo with git-filter-repo once manual smoke testing is
done. Its first section — `# EchoVR Binary Bug Audit`, integer-ID entries — is not
scratch. It is a reverse-engineering audit of echovr.exe: real findings against
real virtual addresses, and in several cases exploit-relevant detail. That
knowledge must be carried into ReVault (the binary-analysis warehouse) BEFORE the
file is rewritten out of history, or it is lost.

Scope: ONLY the integer-ID section (the audit of the original game binary). The
`# NEVR Runtime Source Bugs` section (N-prefixed) audits OUR code, and its durable
record is the commit history — it is not carried here.

Output is deliberately written to a path you pass on the command line, intended
for the scratch dir, NOT committed. The extracted findings ARE the exploit
material the filter-repo is meant to remove; re-committing them into a new public
file would widen exposure right before reducing it. This tool is generic parsing
machinery and carries no findings itself, so it is safe to commit; its output is
not.

Usage:
    tools/extract_binary_audit.py [BUGS.md] [out_dir]
        BUGS.md  defaults to the repo's BUGS.md
        out_dir  defaults to /var/tmp/work-nevr-runtime/binary-audit-export

Produces, in out_dir:
    audit.json          one record per entry: id, severity, title, addresses,
                        source_path, status, description
    revault-plan.md     a human-reviewable VA -> comment plan for ingestion
    UNADDRESSED.md      entries with no VA — these cannot become a ReVault comment
                        and need a human to decide their home

Nothing here writes to ReVault. Ingestion is a separate, reviewed step.
"""
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# The audit is everything from the file's first heading up to the start of the
# NEVR-source section. Anchored on the exact section titles rather than line
# numbers, which drift.
AUDIT_START = "# EchoVR Binary Bug Audit"
AUDIT_END = "# NEVR Runtime Source Bugs"

ENTRY_RE = re.compile(r"^### (\d+)\.\s+(.*)$")
SEVERITY_RE = re.compile(r"^## (High|Medium|Low)\s*$")
# A table row like: | **Address** | `0x140F8E310` (Update_UDPSocket) |
ROW_RE = re.compile(r"^\|\s*\*\*(\w+)\*\*\s*\|\s*(.*?)\s*\|\s*$")
# Any virtual address, in a table cell or in prose. 9+ hex digits after 0x is a
# full VA; the game's image base is 0x140000000, so real VAs are 0x140.. to 0x142..
VA_RE = re.compile(r"0x1[0-9A-Fa-f]{8,}")


def audit_slice(text):
    start = text.index(AUDIT_START)
    end = text.index(AUDIT_END)
    return text[start:end]


def parse(text):
    lines = audit_slice(text).splitlines()
    severity = None
    entries = []
    cur = None

    def flush():
        if cur is not None:
            # Description = the prose lines, table stripped, trimmed.
            desc = "\n".join(cur["_desc"]).strip()
            cur["description"] = desc
            # Collect every VA anywhere in the entry (table cells + prose),
            # de-duplicated, preserving first-seen order.
            seen = []
            for va in VA_RE.findall(cur["_raw"]):
                canon = "0x" + va[2:].upper()
                if canon not in seen:
                    seen.append(canon)
            cur["addresses"] = seen
            del cur["_desc"], cur["_raw"]
            entries.append(cur)

    for ln in lines:
        sev = SEVERITY_RE.match(ln)
        if sev:
            severity = sev.group(1)
            continue
        m = ENTRY_RE.match(ln)
        if m:
            flush()
            cur = {
                "id": int(m.group(1)),
                "severity": severity,
                "title": m.group(2).replace("\\_", "_").strip(),
                "source_path": None,
                "status": None,
                "_desc": [],
                "_raw": "",
            }
            continue
        if cur is None:
            continue
        cur["_raw"] += ln + "\n"
        row = ROW_RE.match(ln)
        if row:
            key, val = row.group(1).lower(), row.group(2)
            if key == "source":
                cur["source_path"] = val.strip("`").strip()
            elif key == "status":
                cur["status"] = val.strip()
            # Address rows contribute to _raw (VA scan) already; no separate field
            # because an entry can carry several addresses in prose too.
            continue
        # A markdown table separator or the empty header row — skip from desc.
        if re.match(r"^\|[\s:|-]*\|$", ln) or re.match(r"^\|\s*\|\s*\|\s*$", ln):
            continue
        cur["_desc"].append(ln)

    flush()
    return entries


def main():
    bugs = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "BUGS.md"
    out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else \
        pathlib.Path("/var/tmp/work-nevr-runtime/binary-audit-export")
    out.mkdir(parents=True, exist_ok=True)

    entries = parse(bugs.read_text())

    (out / "audit.json").write_text(json.dumps(entries, indent=2))

    addressed = [e for e in entries if e["addresses"]]
    unaddressed = [e for e in entries if not e["addresses"]]

    # ReVault ingestion plan: each addressed entry becomes a comment at its
    # PRIMARY address (first VA), cross-referencing any others.
    plan = ["# ReVault ingestion plan — echovr.exe binary audit",
            "",
            f"Source: `BUGS.md` `{AUDIT_START}` section. {len(entries)} entries, "
            f"{len(addressed)} with a virtual address.",
            "",
            "Each entry below becomes a `revault_comment` at its primary VA. This "
            "file is for REVIEW before ingestion — nothing is written to ReVault by "
            "the extractor.",
            ""]
    for e in addressed:
        primary = e["addresses"][0]
        others = ", ".join(e["addresses"][1:])
        plan.append(f"## [{e['severity']}] audit #{e['id']}: {e['title']}")
        plan.append(f"- **primary VA:** `{primary}`")
        if others:
            plan.append(f"- **also references:** {others}")
        if e["source_path"]:
            plan.append(f"- **source:** `{e['source_path']}`")
        if e["status"]:
            plan.append(f"- **status:** {e['status']}")
        if e["description"]:
            plan.append(f"- **finding:** {e['description']}")
        plan.append("")
    (out / "revault-plan.md").write_text("\n".join(plan))

    un = ["# Audit entries with NO virtual address",
          "",
          "These cannot become a ReVault comment at a VA. Each names a pattern or a "
          "call-site count rather than one address; a human decides whether it "
          "becomes a struct/enum note, several comments, or a ReVault bookmark.",
          ""]
    for e in unaddressed:
        un.append(f"- **#{e['id']} [{e['severity']}]** {e['title']}")
        if e["status"]:
            un.append(f"  - status: {e['status']}")
    (out / "UNADDRESSED.md").write_text("\n".join(un))

    # Summary to stdout — the extractor is run by a human who wants the counts.
    print(f"entries         : {len(entries)}")
    print(f"  with a VA     : {len(addressed)}")
    print(f"  without a VA  : {len(unaddressed)}")
    print(f"severity        : " + ", ".join(
        f"{s}={sum(1 for e in entries if e['severity'] == s)}"
        for s in ("High", "Medium", "Low")))
    ids = [e["id"] for e in entries]
    print(f"id range        : {min(ids)}..{max(ids)} ({len(ids)} present)")
    print(f"output          : {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
