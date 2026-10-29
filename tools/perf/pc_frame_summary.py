#!/usr/bin/env python3
"""Summarise one PC run's frame-time probe lines into summary.json (#954).

The runtime (src/runtime/frame/frame_stats.h) writes one Info line per 10 s window while
`diagnostics.frame_stats` is on:

    [NEVR.PERF] frame_stats window_s=10 frames=598 median_us=16667 p95_us=18200 max_us=41000 over_1p5x=3
    overruns=0 [app_dropped= compositor_dropped= app_cpu_us_mean= app_cpu_us_max= app_gpu_us_mean=
    app_gpu_us_max= perf_samples=] elapsed_ms=10003

and one `[NEVR.PERF] frame_stats start mode=... build=...` line at boot. This tool reads the
`nevr-*.jsonl` files of ONE run (every record carries the process's run id, N80), keeps that run's
frame_stats lines, and writes summary.json with the fields the Quest exporter's summary has:
version, median_us, p95_us, fps, dropped (and frames, windows).

The log carries window summaries, not frames, so the run-level figures are built from them:

  frames      sum of the windows' frames (loop-head intervals kept).
  fps         frames / (sum of elapsed_ms / 1000). A window without elapsed_ms counts window_s seconds.
  median_us   the frame-weighted 50th percentile of the windows' median_us.
  p95_us      the frame-weighted 95th percentile of the windows' p95_us. An approximation of the pooled p95:
              the pooled value cannot be recovered from window summaries. max_us (the largest window max)
              and worst_p95_us (the largest window p95) bound it from above.
  dropped     app_dropped + compositor_dropped summed over the windows, or null when no window had a VR
              session (null is "not measured", 0 is "measured, none dropped").
  version     the build= of the run's start line, or null.

A scenario window inside the run is selected with --since / --until (ISO timestamps, compared with each
record's ts). Exit codes: 0 written; 2 bad input (no files, several runs and no --run, a bad timestamp);
3 no frame_stats window in the selection (nothing is written).
"""
import argparse
import datetime
import json
import pathlib
import re
import sys

TAG = "[NEVR.PERF] frame_stats "
FIELD = re.compile(r"(\w+)=(\S+)")
INT_FIELDS = (
    "window_s", "frames", "median_us", "p95_us", "max_us", "over_1p5x", "overruns", "elapsed_ms",
    "app_dropped", "compositor_dropped", "app_cpu_us_mean", "app_cpu_us_max", "app_gpu_us_mean",
    "app_gpu_us_max", "perf_samples",
)


def parse_ts(text):
    """An ISO-8601 timestamp, with or without a trailing Z, as an aware UTC datetime."""
    value = text.strip()
    if value.endswith("Z"):
        value = value[:-1] + "+00:00"
    parsed = datetime.datetime.fromisoformat(value)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=datetime.timezone.utc)
    return parsed


def parse_fields(rest):
    """key=value pairs of a frame_stats line; integer fields become ints, a field that is not an int is dropped."""
    out = {}
    for key, value in FIELD.findall(rest):
        if key in INT_FIELDS:
            try:
                out[key] = int(value)
            except ValueError:
                continue
        else:
            out[key] = value
    return out


def read_records(paths):
    """Every frame_stats record in `paths`: ({'run', 'ts', 'kind', 'fields'}, ...) and the count of lines that
    were not JSON objects. Other lines of the log are not this tool's business and are not counted."""
    records, bad = [], 0
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except ValueError:
                    bad += 1
                    continue
                if not isinstance(record, dict):
                    bad += 1
                    continue
                msg = record.get("msg")
                if not isinstance(msg, str) or TAG not in msg:
                    continue
                rest = msg.split(TAG, 1)[1]
                kind = "start" if rest.startswith("start ") else ("window" if rest.startswith("window_s=") else None)
                if kind is None:
                    continue
                records.append({
                    "run": record.get("run"),
                    "ts": record.get("ts"),
                    "kind": kind,
                    "fields": parse_fields(rest),
                })
    return records, bad


def weighted_percentile(pairs, pct):
    """The value at which the cumulative weight first reaches pct percent of the total. pairs: (value, weight)."""
    pairs = sorted((v, w) for v, w in pairs if w > 0)
    total = sum(w for _, w in pairs)
    if total == 0:
        return None
    running = 0
    for value, weight in pairs:
        running += weight
        if running * 100 >= pct * total:
            return value
    return pairs[-1][0]


def summarise(records, run=None, since=None, until=None):
    """(summary dict, None), or (None, error string). `run` selects a run id; with none given the records
    must hold exactly one."""
    runs = sorted({r["run"] for r in records if r["run"] is not None})
    if run is None:
        if len(runs) > 1:
            return None, "several runs in these files (%s); pass --run" % ", ".join(runs)
        run = runs[0] if runs else None
    selected = [r for r in records if r["run"] == run]
    start_line = next((r for r in selected if r["kind"] == "start"), None)
    windows = []
    for r in selected:
        if r["kind"] != "window":
            continue
        if since is not None or until is not None:
            if r["ts"] is None:
                continue
            when = parse_ts(r["ts"])
            if since is not None and when < since:
                continue
            if until is not None and when > until:
                continue
        windows.append(r["fields"])
    complete = [w for w in windows if all(k in w for k in ("frames", "median_us", "p95_us", "max_us"))]
    if not complete:
        return None, "no frame_stats window in run %s for this selection" % (run,)

    frames = sum(w["frames"] for w in complete)
    seconds = sum(w["elapsed_ms"] / 1000.0 if "elapsed_ms" in w else float(w.get("window_s", 10)) for w in complete)
    vr = [w for w in complete if "app_dropped" in w]
    dropped = sum(w["app_dropped"] + w.get("compositor_dropped", 0) for w in vr) if vr else None
    summary = {
        "version": start_line["fields"].get("build") if start_line else None,
        "run": run,
        "mode": start_line["fields"].get("mode") if start_line else None,
        "host": start_line["fields"].get("host") if start_line else None,
        "median_us": weighted_percentile([(w["median_us"], w["frames"]) for w in complete], 50),
        "p95_us": weighted_percentile([(w["p95_us"], w["frames"]) for w in complete], 95),
        "max_us": max(w["max_us"] for w in complete),
        "worst_p95_us": max(w["p95_us"] for w in complete),
        "fps": round(frames / seconds, 2) if seconds > 0 and frames > 0 else None,
        "dropped": dropped,
        "app_dropped": sum(w["app_dropped"] for w in vr) if vr else None,
        "compositor_dropped": sum(w.get("compositor_dropped", 0) for w in vr) if vr else None,
        "over_1p5x": sum(w.get("over_1p5x", 0) for w in complete),
        "overruns": sum(w.get("overruns", 0) for w in complete),
        "frames": frames,
        "windows": len(complete),
        "seconds": round(seconds, 3),
    }
    if len(complete) != len(windows):
        summary["incomplete_windows"] = len(windows) - len(complete)
    return summary, None


def resolve_paths(args):
    paths = []
    for item in args:
        p = pathlib.Path(item)
        if p.is_dir():
            paths.extend(sorted(p.glob("nevr-*.jsonl")))
        elif p.is_file():
            paths.append(p)
    return paths


def main(argv=None):
    ap = argparse.ArgumentParser(description="Summarise one run's [NEVR.PERF] frame_stats lines into summary.json.")
    ap.add_argument("logs", nargs="+", help="nevr-*.jsonl files of one run, or a directory holding them")
    ap.add_argument("--run", help="the run id (the `run` field of every record); needed when the files hold several")
    ap.add_argument("--since", help="ISO timestamp: only windows logged at or after it")
    ap.add_argument("--until", help="ISO timestamp: only windows logged at or before it")
    ap.add_argument("--out", default="summary.json", help="where to write the summary (default ./summary.json)")
    args = ap.parse_args(argv)

    paths = resolve_paths(args.logs)
    if not paths:
        print("pc_frame_summary: no nevr-*.jsonl file in %s" % " ".join(args.logs), file=sys.stderr)
        return 2
    try:
        since = parse_ts(args.since) if args.since else None
        until = parse_ts(args.until) if args.until else None
    except ValueError as exc:
        print("pc_frame_summary: bad timestamp: %s" % exc, file=sys.stderr)
        return 2
    records, bad = read_records(paths)
    summary, error = summarise(records, args.run, since, until)
    if summary is None:
        print("pc_frame_summary: %s" % error, file=sys.stderr)
        return 3 if error.startswith("no frame_stats") else 2
    summary["skipped_lines"] = bad
    pathlib.Path(args.out).write_text(json.dumps(summary, indent=2) + "\n")
    print("pc_frame_summary: wrote %s (%d windows, %d frames)" % (args.out, summary["windows"], summary["frames"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
