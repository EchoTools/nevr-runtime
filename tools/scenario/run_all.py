#!/usr/bin/env python3
"""Run every social scenario, one after another, and print one PASS/FAIL table.

Each scenario runs exactly as `run_scenario.py` runs it (its own client launch, event-driven waits,
its own run folder); the suite's folder holds one sub-folder per scenario plus `suite.md` and
`suite.json`. Every `*.yaml` in tools/scenario/scenarios/ is in the suite, so a new scenario cannot
be left out by forgetting a list. Exit 0 only if every scenario passed.
docs/design/2026-10-01-social-scenario-harness.md
"""

import argparse
import datetime
import json
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_scenario  # noqa: E402

SCENARIOS = pathlib.Path(__file__).resolve().parent / "scenarios"


def suite_table(rows: list[dict]) -> str:
    lines = ["| # | scenario | result | s | run folder |", "|---|---|---|---|---|"]
    for i, row in enumerate(rows, 1):
        lines.append(f"| {i} | {row['scenario']} | {row['result']} | {row['seconds']:.0f} | {row['folder']} |")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dll", type=pathlib.Path, default=run_scenario.DEFAULT_DLL)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args(argv)

    scenarios = sorted(SCENARIOS.glob("*.yaml"))
    if not scenarios:
        print(f"ERROR: no scenarios in {SCENARIOS}", file=sys.stderr)
        return 2
    stamp = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
    out = args.out or run_scenario.SCRATCH / f"{stamp}-suite"
    out.mkdir(parents=True, exist_ok=True)
    print(f"suite: {len(scenarios)} scenarios -> {out}", flush=True)

    rows = []
    for path in scenarios:
        name = path.stem
        folder = out / name
        print(f"suite: running {name}", flush=True)
        started = time.monotonic()
        code = run_scenario.main([str(path), "--dll", str(args.dll), "--out", str(folder)])
        seconds = time.monotonic() - started
        result_file = folder / "result.json"
        passed = False
        if result_file.is_file():
            passed = bool(json.loads(result_file.read_text()).get("passed")) and code == 0
        rows.append({"scenario": name, "result": "PASS" if passed else "FAIL", "seconds": seconds,
                     "exit": code, "folder": str(folder)})
        print(f"suite: {name} {'PASS' if passed else 'FAIL'} exit={code} seconds={seconds:.0f}", flush=True)

    all_passed = all(row["result"] == "PASS" for row in rows)
    md = f"# scenario suite: {'PASS' if all_passed else 'FAIL'} ({sum(r['result'] == 'PASS' for r in rows)}/{len(rows)})\n\n"
    md += suite_table(rows) + "\n"
    (out / "suite.md").write_text(md)
    (out / "suite.json").write_text(json.dumps({"passed": all_passed, "dll": str(args.dll), "scenarios": rows}, indent=2))
    print(md)
    print(f"suite folder: {out}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
