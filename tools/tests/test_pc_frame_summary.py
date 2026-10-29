"""tools/perf/pc_frame_summary.py: one run's [NEVR.PERF] frame_stats lines in, summary.json out (#954)."""
import json
import pathlib
import re
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "perf"))
import pc_frame_summary as pfs  # noqa: E402

RUN_A = "20261026T101500Z-1111"
RUN_B = "20261026T111500Z-2222"


def window_msg(frames=600, median=16667, p95=18000, mx=40000, over=2, overruns=0, elapsed=10000, vr=None):
    msg = ("[NEVR.PERF] frame_stats window_s=10 frames=%d median_us=%d p95_us=%d max_us=%d over_1p5x=%d "
           "overruns=%d" % (frames, median, p95, mx, over, overruns))
    if vr is not None:
        msg += (" app_dropped=%d compositor_dropped=%d app_cpu_us_mean=5000 app_cpu_us_max=9000 "
                "app_gpu_us_mean=6000 app_gpu_us_max=10000 perf_samples=700" % vr)
    return msg + " elapsed_ms=%d" % elapsed


def start_msg(build="v5.6.0-3-gabc1234", mode="full", host="client"):
    return ("[NEVR.PERF] frame_stats start mode=%s recognised=yes host=%s build=%s frame_ring=armed "
            "perf_hook=installed" % (mode, host, build))


def rec(msg, run=RUN_A, ts="2026-10-26T10:15:30.000Z", level="info"):
    return json.dumps({"ts": ts, "run": run, "level": level, "msg": msg})


def write_log(directory, name, lines):
    path = pathlib.Path(directory) / name
    path.write_text("\n".join(lines) + "\n")
    return path


class ParseTest(unittest.TestCase):
    def test_a_window_line_parses_every_field_as_an_integer(self):
        rest = window_msg(vr=(4, 1)).split(pfs.TAG, 1)[1]
        fields = pfs.parse_fields(rest)
        self.assertEqual(fields["frames"], 600)
        self.assertEqual(fields["p95_us"], 18000)
        self.assertEqual(fields["over_1p5x"], 2)
        self.assertEqual(fields["app_dropped"], 4)
        self.assertEqual(fields["compositor_dropped"], 1)
        self.assertEqual(fields["elapsed_ms"], 10000)

    def test_the_lines_the_cpp_test_expects_parse(self):
        # The C++ test pins the exact text the runtime writes; this tool must read that text.
        source = (REPO / "src/runtime/tests/test_frame_stats.cpp").read_text()
        lines = []
        for block in re.findall(r"EXPECT_STREQ\(\s*((?:\"[^\"]*\"\s*)+),", source):
            lines.append("".join(re.findall(r"\"([^\"]*)\"", block)))
        windows = [x for x in lines if "window_s=" in x]
        self.assertEqual(len(windows), 2, lines)
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, "nevr-a.jsonl", [rec(x) for x in windows])
            records, bad = pfs.read_records([path])
        self.assertEqual(bad, 0)
        self.assertEqual([r["kind"] for r in records], ["window", "window"])
        self.assertEqual(records[0]["fields"]["frames"], 598)
        self.assertEqual(records[0]["fields"]["elapsed_ms"], 10003)
        self.assertEqual(records[1]["fields"]["app_gpu_us_max"], 10400)
        starts = [x for x in lines if " start " in x]
        self.assertEqual(len(starts), 2)
        self.assertEqual(pfs.parse_fields(starts[0].split(pfs.TAG, 1)[1])["build"], "v5.6.0-3-gabc1234")

    def test_other_lines_and_bad_json_are_not_windows(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, "nevr-a.jsonl", [
                rec("[NEVR.PATCH] per-frame tick ALIVE"),
                "not json at all",
                "[1, 2]",
                rec("[NEVR.PERF] frame_stats something_else=1"),
                rec(window_msg()),
            ])
            records, bad = pfs.read_records([path])
        self.assertEqual(len(records), 1)
        self.assertEqual(bad, 2)


class PercentileTest(unittest.TestCase):
    def test_weighted_percentile_follows_the_frames(self):
        pairs = [(10, 1), (20, 1), (30, 8)]
        self.assertEqual(pfs.weighted_percentile(pairs, 50), 30)  # 8 of 10 frames are in the 30
        self.assertEqual(pfs.weighted_percentile(pairs, 10), 10)
        self.assertEqual(pfs.weighted_percentile(pairs, 20), 20)
        self.assertEqual(pfs.weighted_percentile(pairs, 95), 30)

    def test_zero_weight_and_empty_have_no_percentile(self):
        self.assertIsNone(pfs.weighted_percentile([], 50))
        self.assertIsNone(pfs.weighted_percentile([(5, 0)], 50))
        self.assertEqual(pfs.weighted_percentile([(5, 0), (7, 3)], 50), 7)


class SummariseTest(unittest.TestCase):
    def records(self, lines):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, "nevr-a.jsonl", lines)
            return pfs.read_records([path])[0]

    def test_run_figures_come_from_the_windows_weighted_by_frames(self):
        records = self.records([
            rec(start_msg()),
            rec(window_msg(frames=600, median=16000, p95=17000, mx=30000, over=1, overruns=0, elapsed=10000)),
            rec(window_msg(frames=300, median=20000, p95=25000, mx=90000, over=4, overruns=2, elapsed=10000)),
        ])
        summary, error = pfs.summarise(records)
        self.assertIsNone(error)
        self.assertEqual(summary["version"], "v5.6.0-3-gabc1234")
        self.assertEqual(summary["frames"], 900)
        self.assertEqual(summary["windows"], 2)
        self.assertEqual(summary["median_us_est"], 16000)  # 600 of 900 frames are in the first window
        self.assertEqual(summary["p95_us_est"], 25000)     # the 95th percentile of the frames reaches the second
        self.assertEqual(summary["max_us"], 90000)
        self.assertEqual(summary["worst_p95_us"], 25000)
        self.assertEqual(summary["over_1p5x"], 5)
        self.assertEqual(summary["overruns"], 2)
        self.assertEqual(summary["fps"], 45.0)         # 900 frames over 20 s
        self.assertEqual(summary["seconds"], 20.0)
        self.assertIsNone(summary["dropped"])          # no VR window: not measured

    def test_fps_divides_by_the_real_window_length_not_the_nominal_ten_seconds(self):
        records = self.records([rec(window_msg(frames=1000, elapsed=12500))])
        summary, _ = pfs.summarise(records)
        self.assertEqual(summary["fps"], 80.0)

    def test_a_window_without_elapsed_ms_counts_its_nominal_seconds(self):
        line = window_msg(frames=500).replace(" elapsed_ms=10000", "")
        summary, _ = pfs.summarise(self.records([rec(line)]))
        self.assertEqual(summary["fps"], 50.0)

    def test_dropped_sums_app_and_compositor_over_the_vr_windows(self):
        records = self.records([
            rec(window_msg(vr=(4, 1))),
            rec(window_msg()),               # a window with no VR fields adds nothing
            rec(window_msg(vr=(2, 0))),
        ])
        summary, _ = pfs.summarise(records)
        self.assertEqual(summary["dropped"], 7)
        self.assertEqual(summary["app_dropped"], 6)
        self.assertEqual(summary["compositor_dropped"], 1)

    def test_zero_drops_in_a_vr_run_is_zero_not_null(self):
        summary, _ = pfs.summarise(self.records([rec(window_msg(vr=(0, 0)))]))
        self.assertEqual(summary["dropped"], 0)

    def test_percentiles_are_named_as_estimates_of_window_summaries(self):
        # Two 1000-frame windows: 940 x 1000 us + 60 x 151000 us (median 1000, p95 151000) and 1000 x 10000 us
        # (median 10000, p95 10000). Both pooled percentiles are 10000, which window summaries cannot recover, so
        # the file must not present these figures as run percentiles.
        records = self.records([
            rec(window_msg(frames=1000, median=1000, p95=151000, mx=151000, elapsed=10000)),
            rec(window_msg(frames=1000, median=10000, p95=10000, mx=10000, elapsed=10000)),
        ])
        summary, error = pfs.summarise(records)
        self.assertIsNone(error)
        self.assertNotIn("median_us", summary)
        self.assertNotIn("p95_us", summary)
        self.assertEqual(summary["percentile_basis"], "window_summaries")
        self.assertEqual(summary["median_us_est"], 1000)
        self.assertEqual(summary["p95_us_est"], 151000)

    def test_windows_with_no_frames_leave_the_percentiles_null(self):
        records = self.records([rec(window_msg(frames=0, median=0, p95=0, mx=0, vr=(1, 0)))])
        summary, _ = pfs.summarise(records)
        self.assertIsNone(summary["median_us_est"])
        self.assertIsNone(summary["fps"])
        self.assertEqual(summary["dropped"], 1)

    def test_several_runs_without_a_choice_is_an_error_and_run_selects_one(self):
        records = self.records([
            rec(window_msg(frames=100), run=RUN_A),
            rec(window_msg(frames=200), run=RUN_B),
        ])
        summary, error = pfs.summarise(records)
        self.assertIsNone(summary)
        self.assertIn("pass --run", error)
        self.assertIn(RUN_A, error)
        self.assertIn(RUN_B, error)
        summary, error = pfs.summarise(records, run=RUN_B)
        self.assertIsNone(error)
        self.assertEqual(summary["frames"], 200)
        self.assertEqual(summary["run"], RUN_B)

    def test_since_and_until_select_the_scenario_window(self):
        records = self.records([
            rec(window_msg(frames=100), ts="2026-10-26T10:00:10.000Z"),
            rec(window_msg(frames=200), ts="2026-10-26T10:00:20.000Z"),
            rec(window_msg(frames=400), ts="2026-10-26T10:00:30.000Z"),
        ])
        since, until = pfs.parse_ts("2026-10-26T10:00:15Z"), pfs.parse_ts("2026-10-26T10:00:25Z")
        summary, _ = pfs.summarise(records, since=since)
        self.assertEqual(summary["frames"], 600)
        summary, _ = pfs.summarise(records, until=until)
        self.assertEqual(summary["frames"], 300)
        summary, _ = pfs.summarise(records, since=since, until=until)
        self.assertEqual(summary["frames"], 200)

    def test_no_window_in_the_selection_is_an_error(self):
        records = self.records([rec(start_msg())])
        summary, error = pfs.summarise(records)
        self.assertIsNone(summary)
        self.assertTrue(error.startswith("no frame_stats"))

    def test_a_window_missing_a_required_field_is_counted_and_left_out(self):
        broken = "[NEVR.PERF] frame_stats window_s=10 frames=50 median_us=1"
        summary, _ = pfs.summarise(self.records([rec(window_msg(frames=100)), rec(broken)]))
        self.assertEqual(summary["frames"], 100)
        self.assertEqual(summary["incomplete_windows"], 1)


class MainTest(unittest.TestCase):
    def test_main_writes_the_summary_fields_the_quest_summary_has(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_log(tmp, "nevr-20261026.jsonl", [
                rec(start_msg()),
                rec(window_msg(vr=(3, 1))),
            ])
            write_log(tmp, "other.jsonl", [rec(window_msg(frames=99999))])  # not a nevr-*.jsonl
            out = pathlib.Path(tmp) / "summary.json"
            self.assertEqual(pfs.main([tmp, "--out", str(out)]), 0)
            summary = json.loads(out.read_text())
        for key in ("version", "median_us_est", "p95_us_est", "fps", "dropped", "frames", "windows"):
            self.assertIn(key, summary)
        self.assertEqual(summary["frames"], 600)
        self.assertEqual(summary["dropped"], 4)
        self.assertEqual(summary["version"], "v5.6.0-3-gabc1234")
        self.assertEqual(summary["skipped_lines"], 0)

    def test_main_with_no_window_exits_3_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, "nevr-a.jsonl", [rec(start_msg())])
            out = pathlib.Path(tmp) / "summary.json"
            self.assertEqual(pfs.main([str(path), "--out", str(out)]), 3)
            self.assertFalse(out.exists())

    def test_main_with_no_files_exits_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(pfs.main([tmp, "--out", str(pathlib.Path(tmp) / "s.json")]), 2)

    def test_main_with_several_runs_exits_2_and_a_bad_timestamp_exits_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_log(tmp, "nevr-a.jsonl", [rec(window_msg(), run=RUN_A), rec(window_msg(), run=RUN_B)])
            out = pathlib.Path(tmp) / "summary.json"
            self.assertEqual(pfs.main([str(path), "--out", str(out)]), 2)
            self.assertEqual(pfs.main([str(path), "--run", RUN_A, "--since", "yesterday", "--out", str(out)]), 2)
            self.assertFalse(out.exists())
            self.assertEqual(pfs.main([str(path), "--run", RUN_A, "--out", str(out)]), 0)


if __name__ == "__main__":
    unittest.main()
