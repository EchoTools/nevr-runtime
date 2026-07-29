#!/usr/bin/env python3
"""docs/standards/logging.md rules that are mechanically checkable.

A SHALL that could be a check and is not is a defect in the documentation
(AGENTS.md §Normative language). These are the rules whose violation has already
cost something.

Rule: no INFO-level logging inside a per-frame hot path. DispatchPerFrameWork
runs at ~125Hz; an ungated Info line there floods the log and buries real events.
One-shot and interval-gated lines are exempt — they are the intended pattern.

Judged PER OCCURRENCE. An earlier shell version ran `grep -B4` over every Info
line at once, so a legitimately-gated line's guard exempted an unguarded one four
lines away. Falsification caught it; that is why this is python.

Exit 0 = pass. Exit 1 = violation.
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# (file, function, regex matching an acceptable guard in the preceding lines)
HOT_PATHS = [
    ("src/runtime/patch/binary_bug_fixes.cpp", "DispatchPerFrameWork",
     r"if \(t == 1\)|% 3750|s_ticks|first call"),
]
LOOKBACK = 6


def check_hot_path(path: str, func: str, guard: str, problems: list) -> None:
    src = (REPO / path).read_text().split("\n")
    start = next((i for i, l in enumerate(src) if l.lstrip().startswith(("static void " + func, "void " + func))), None)
    if start is None:
        problems.append(f"{path}: {func}() not found — the hot-path rule cannot be checked")
        return
    end = next((i for i in range(start + 1, len(src)) if src[i] == "}"), len(src))

    for i in range(start, end):
        line = src[i]
        if re.match(r"\s*(//|\*|/\*)", line):
            continue
        if "LogLevel::Info" not in line:
            continue
        window = "\n".join(src[max(start, i - LOOKBACK):i])
        if not re.search(guard, window):
            problems.append(
                f"{path}:{i + 1}: unguarded Info log inside {func}() — that path runs "
                f"~125Hz. Gate it on a first-call or interval test, or use Debug.")


def main() -> int:
    problems: list[str] = []
    for path, func, guard in HOT_PATHS:
        check_hot_path(path, func, guard, problems)
    for p in problems:
        print(f"log-rules: FAIL {p}", file=sys.stderr)
    if problems:
        print("\nlog-rules: see docs/standards/logging.md — Level Guidelines.", file=sys.stderr)
        return 1
    print("log-rules: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
