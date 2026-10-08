#!/usr/bin/env python3
"""Quest sentinel: OpenSSL / libcurl / token auth must not reach it unguarded.

The game's own libraries (libr15, libpnsrad, libpnsovr, libpnsradmatchmaking) export about
2411 OpenSSL and libcurl symbols (an older OpenSSL) and have the sentinel as DT_NEEDED. A sentinel
that carried its own OpenSSL and exported it could have its calls bound to the game's copy, or
the game's to ours. The guard is `-Wl,--exclude-libs,ALL` on the sentinel's link plus the export
allowlist test (`TestExportAllowlist` in tests/quest: only JNI_OnLoad and nevr_sentinel_marker).

This reads the Quest CMake files (src/quest/**/CMakeLists.txt and *.cmake), builds the target
link graph from `target_link_libraries`, and walks it from the sentinel target. If the closure
reaches token auth, libcurl or OpenSSL it requires:
  * `--exclude-libs,ALL` in a `target_link_options(ovrplatformloader ...)` (comments ignored), and
  * `func TestExportAllowlist` in tests/quest/elf_groundtruth_test.go, not skipped.
It is a source-level check; the configured build graph is checked by `TestSentinelLinkLineGuard`
(tests/quest, `just test-android`), which looks at the real link line of the built artifact.

Usage: verify_quest_sentinel_link.py [repo_root]    exit 0 ok, 1 violation, 2 cannot read.
"""
import re
import sys
from pathlib import Path

SENTINEL = "ovrplatformloader"
HEAVY = re.compile(r"^(nevr_quest_token_auth|CURL::.*|OpenSSL::.*|CURL|OpenSSL|libcurl|libssl|libcrypto|curl|ssl|crypto)$")
KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE", "BEFORE"}
COMMAND = re.compile(r"\b(target_link_libraries|target_link_options)\s*\((.*?)\)", re.S | re.I)
FLAG = re.compile(r"--exclude-libs,ALL")


def strip_comments(text: str) -> str:
    out = []
    for line in text.splitlines():
        in_quote = False
        cut = len(line)
        for i, ch in enumerate(line):
            if ch == '"':
                in_quote = not in_quote
            elif ch == "#" and not in_quote:
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


def read_graph(src_quest: Path):
    edges = {}
    options = {}
    files = sorted(list(src_quest.rglob("CMakeLists.txt")) + list(src_quest.rglob("*.cmake")))
    if not files:
        raise OSError(f"no CMake files under {src_quest}")
    for f in files:
        text = strip_comments(f.read_text())
        for m in COMMAND.finditer(text):
            cmd = m.group(1).lower()
            tokens = [t.strip('"') for t in m.group(2).split()]
            if not tokens:
                continue
            target, items = tokens[0], [t for t in tokens[1:] if t not in KEYWORDS]
            if cmd == "target_link_libraries":
                edges.setdefault(target, set()).update(i for i in items if not i.startswith(("-", "$")))
            else:
                options.setdefault(target, []).extend(items)
    return edges, options


def closure(edges, root):
    seen, stack = set(), [root]
    while stack:
        node = stack.pop()
        for nxt in edges.get(node, ()):
            if nxt not in seen:
                seen.add(nxt)
                stack.append(nxt)
    return seen


def allowlist_problem(go_file: Path):
    if not go_file.exists():
        return f"{go_file} is missing"
    text = go_file.read_text()
    m = re.search(r"^func TestExportAllowlist\(.*?\n}", text, re.S | re.M)
    if not m:
        return "func TestExportAllowlist is missing"
    if re.search(r"\bt\.Skip(f|Now)?\(", m.group(0)):
        return "TestExportAllowlist skips itself"
    return None


def check(root: Path):
    edges, options = read_graph(root / "src" / "quest")
    reached = closure(edges, SENTINEL)
    heavy = sorted(n for n in reached if HEAVY.match(n))
    if not heavy:
        return 0, f"sentinel-link: OK (the sentinel's link closure reaches no OpenSSL/libcurl/token auth; {len(reached)} targets)"
    problems = []
    if not any(FLAG.search(o) for o in options.get(SENTINEL, [])):
        problems.append("target_link_options(ovrplatformloader ...) has no -Wl,--exclude-libs,ALL")
    p = allowlist_problem(root / "tests" / "quest" / "elf_groundtruth_test.go")
    if p:
        problems.append(p)
    if problems:
        return 1, ("sentinel-link: FAIL -- the sentinel links " + ", ".join(heavy) + " but " + "; ".join(problems) +
                   ". It would export OpenSSL/libcurl next to the game's older copy (ADR 0003).")
    return 0, "sentinel-link: OK (links " + ", ".join(heavy) + " with --exclude-libs,ALL and a live TestExportAllowlist)"


def main(argv):
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    try:
        code, message = check(root)
    except OSError as e:
        print(f"sentinel-link: cannot read the Quest build files: {e}", file=sys.stderr)
        return 2
    print(message, file=sys.stderr if code else sys.stdout)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
