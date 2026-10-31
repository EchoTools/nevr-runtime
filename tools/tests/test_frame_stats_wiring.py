"""#954: the wiring of the PC frame-time probe, which the host GTest (test_frame_stats) cannot reach.

The GTest drives src/runtime/frame/frame_stats.h. These checks read the places that header's functions are
CALLED from and the one hook the probe installs. They are source-shape checks (evidence ladder rank 3): they
fail when a call is removed or moved, and say nothing about whether the hook runs. Only a run of the game
shows that (`[NEVR.PERF] frame_stats` with frames > 0).
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_runtime_lifecycle_invariants import extract_braced_function, strip_comments  # noqa: E402


def read(rel):
    return (ROOT / rel).read_text()


class LoopHeadHookTest(unittest.TestCase):
    def body(self):
        return extract_braced_function(read("src/runtime/patch/binary_bug_fixes.cpp"),
                                       "static uint64_t __fastcall GetTimeMicrosecondsHook(")

    def test_the_return_address_is_read_in_the_hook_body_and_compared_with_the_loop_head(self):
        body = self.body()
        self.assertRegex(
            body,
            r"nevr_frame_stats::IsLoopHeadReturn\(\s*reinterpret_cast<uintptr_t>\(\s*__builtin_return_address\(0\)\s*\)"
            r"\s*,\s*g_base\s*\)")

    def test_the_reading_is_pushed_only_inside_that_test(self):
        body = self.body()
        self.assertRegex(
            body,
            r"if\s*\(\s*nevr_frame_stats::IsLoopHeadReturn\([^;]*\)\s*\)\s*\{\s*"
            r"nevr_frame_stats::OnLoopHead\(\s*QpcMicroseconds\(\)\s*\)\s*;\s*\}")
        self.assertEqual(body.count("OnLoopHead("), 1)

    def test_the_reading_is_taken_before_the_time_override_return(self):
        body = self.body()
        push = body.index("OnLoopHead(")
        self.assertLess(push, body.index("override_flag"), "an override clock must not hide a slow frame")
        self.assertLess(body.index("Mark(nevr_hook_liveness::kGetTimeMicroseconds)"), push)

    def test_the_hook_logs_nothing(self):
        self.assertNotIn("Log(", self.body())

    def test_the_dispatcher_call_is_still_there_after_the_push(self):
        body = self.body()
        self.assertLess(body.index("OnLoopHead("), body.index("nevr_frame::DispatchPerFrameWork(nowUs)"))


class PollWiringTest(unittest.TestCase):
    def test_poll_runs_from_the_one_dispatcher_beside_the_window_watch(self):
        body = extract_braced_function(read("src/runtime/frame/tick.cpp"), "void DispatchPerFrameWork(")
        self.assertEqual(body.count("nevr_frame_stats::Poll(nowUs);"), 1)
        self.assertLess(body.index("nevr_window_watch::Poll("), body.index("nevr_frame_stats::Poll("))
        self.assertLess(body.index("nevr_frame_stats::Poll("), body.index("TickPlugins(&gctx)"))
        self.assertNotRegex(body, r"nevr_frame_stats::[A-Za-z]+\([^)]*\)[^;]*Log\(")

    def test_no_other_file_polls_the_probe(self):
        callers = []
        for path in (ROOT / "src/runtime").rglob("*.cpp"):
            if "nevr_frame_stats::Poll(" in strip_comments(path.read_text()):
                callers.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(callers, ["src/runtime/frame/tick.cpp"])


class BootWiringTest(unittest.TestCase):
    def test_the_switch_is_read_then_configured_before_the_plugins_load(self):
        boot = strip_comments(read("src/runtime/lifecycle/boot.cpp"))
        read_at = boot.index("NevrCfgFrameStatsMode(&recognised)")
        configure_at = boot.index("nevr_frame_stats::Configure(")
        plugins_at = boot.index("LoadPlugins();", configure_at)
        self.assertLess(read_at, configure_at)
        self.assertLess(configure_at, plugins_at)
        self.assertRegex(boot, r"nevr_frame_stats::Configure\(\s*reinterpret_cast<std::uintptr_t>\("
                               r"EchoVR::g_GameBaseAddress\)\s*,\s*g_isServer\s*!=\s*FALSE\s*,\s*frameStats\s*,"
                               r"\s*recognised\s*!=\s*0\s*\)")
        self.assertEqual(boot.count("nevr_frame_stats::Configure("), 1)

    def test_the_config_accessor_reads_the_documented_key_through_the_pure_reader(self):
        service_map = strip_comments(read("src/runtime/lifecycle/service_map.cpp"))
        self.assertRegex(service_map, r'cfg\.GetString\(\s*"diagnostics\.frame_stats"\s*\)')
        config = extract_braced_function(read("src/runtime/lifecycle/service_config.cpp"),
                                         "int NevrCfgFrameStatsMode(")
        self.assertRegex(config, r"nevr_cfg::FrameStatsMode\(\s*NevrCfg\(\)\s*,\s*&known\s*\)")
        example = read("docs/reference/example-config.yaml")
        self.assertRegex(example, r"(?m)^\s*# frame_stats: true\s*$")


class RuntimeSideTest(unittest.TestCase):
    def source(self):
        return read("src/runtime/frame/frame_stats.cpp")

    def test_the_perf_hook_calls_the_original_once_and_copies_only_on_success(self):
        body = extract_braced_function(self.source(), "int32_t __fastcall PerfStatsHook(")
        self.assertTrue(body.lstrip("{ \n").startswith("nevr_hook_liveness::Mark(nevr_hook_liveness::kOvrGetPerfStats);"))
        self.assertEqual(len(re.findall(r"g_originalPerfStats\(", body)), 1, "a second call would race the session")
        self.assertRegex(body, r"const int32_t result = g_originalPerfStats\(session, outStats\);")
        self.assertRegex(body, r"if \(ShouldCopyPerf\(result, outStats, g_perfOn\.load\(std::memory_order_relaxed\)\)\)")
        self.assertRegex(body, r"CapturePerf\(outStats, g_perfRing, g_perfRejected\);")
        self.assertLess(body.index("g_originalPerfStats("), body.index("CapturePerf("))
        self.assertRegex(body, r"return result;\s*\}$")
        self.assertNotIn("Log(", body)

    def test_the_loop_head_push_logs_nothing_and_is_gated_on_the_flag(self):
        body = extract_braced_function(self.source(), "void OnLoopHead(")
        self.assertNotIn("Log(", body)
        self.assertRegex(body, r"if \(!ShouldRecordLoopHead\(nowUs, g_frameRingOn\.load\(std::memory_order_relaxed\)\)\) return;")
        self.assertRegex(body, r"RecordLoopHead\(g_frameRing, nowUs\);")

    def test_the_perf_hook_is_validated_then_installed_and_never_on_a_server(self):
        body = extract_braced_function(self.source(), "const char* InstallPerfStatsHook(")
        self.assertLess(body.index("if (isServer) return"), body.index("ValidatePrologue("))
        self.assertLess(body.index("ValidatePrologue("), body.index("PatchDetour("))
        self.assertIn("kPerfStatsPrologue, sizeof(kPerfStatsPrologue)", body)
        self.assertRegex(body, r"PatchDetour\(&g_originalPerfStats, [^,]*PerfStatsHook\), \"ovr_GetPerfStats\"\)")
        self.assertRegex(body, r"g_originalPerfStats = reinterpret_cast<PerfStatsFn>\(gameBase \+ "
                               r"nevr_patch_addresses::OVR_GET_PERF_STATS\);")

    def test_the_prologue_bytes_are_the_seven_the_manifest_pins(self):
        text = strip_comments(self.source())
        m = re.search(r"kPerfStatsPrologue\[7\] = \{([^}]*)\}", text)
        self.assertIsNotNone(m)
        pinned = "".join(f"{int(b, 16):02x}" for b in m.group(1).split(","))
        self.assertEqual(pinned, "488b05a154d800")

    def test_configure_installs_the_perf_copy_whatever_the_switch_says_and_logs_the_start_line_always(self):
        body = extract_braced_function(self.source(), "void Configure(")
        self.assertRegex(body, r"const char\* perfHook = InstallPerfStatsHook\(gameBase, isServer\);")
        install = body.index("InstallPerfStatsHook(")
        # not inside the `if (mode == Mode::Full)` block: that block closes before the install
        self.assertLess(body.index("if (mode == Mode::Full) {"), install)
        self.assertEqual(body[body.index("if (mode == Mode::Full) {"):install].count("{"),
                         body[body.index("if (mode == Mode::Full) {"):install].count("}"))
        self.assertRegex(body, r"g_perfOn\.store\(KeepsPerfStats\(mode\), ")
        self.assertRegex(body, r"g_mode\.store\(mode, ")
        self.assertRegex(body, r"FormatStartLine\(line, sizeof\(line\), mode, recognised,")
        self.assertRegex(body, r"Log\(EchoVR::LogLevel::Info, \"%s\", line\);")
        self.assertRegex(body, r"callSiteMatches = LoopHeadCallMatches\(site\);")
        self.assertRegex(body, r"const char\* frameRing = FrameRingState\(mode, callSiteMatches\);")
        self.assertRegex(body, r"g_frameRingOn\.store\(ArmsFrameRing\(mode, callSiteMatches\), ")

    def test_poll_returns_while_off_and_writes_one_info_line_per_window(self):
        body = extract_braced_function(self.source(), "void Poll(")
        self.assertLess(body.index("== Mode::Off) return;"), body.index("Drain("))
        self.assertRegex(body, r"Drain\(g_frameRing, g_perfRing, g_perfRejected, g_drain, g_collector\);")
        self.assertRegex(body, r"if \(!g_clock\.Tick\(nowUs, &elapsedMs\)\) return;")
        self.assertEqual(len(re.findall(r"Log\(", body)), 1)
        self.assertRegex(body, r"Log\(EchoVR::LogLevel::Info, \"%s\", line\);")
        self.assertRegex(body, r"g_collector\.Finish\(elapsedMs\)")


class LivenessRowTest(unittest.TestCase):
    def test_the_perf_stats_row_says_it_is_expected_only_in_a_pcvr_session(self):
        header = read("src/runtime/hook/hook_liveness.h")
        self.assertRegex(header, r"kBroadcasterReceiveLocal,\s*kOvrGetPerfStats,\s*kApplyGfxSettings,\s*kCount")
        cpp = strip_comments(read("src/runtime/hook/hook_liveness.cpp"))
        self.assertIn('{"ovr_GetPerfStats", "PCVR session only", false},', cpp)
        guaranteed_absent = re.findall(r'\{"[^"]*", "[^"]*", true[},]', cpp)  # the third field is guaranteedAbsentOnServer
        self.assertEqual(len(guaranteed_absent), 1, "a second guaranteedAbsentOnServer row would be swallowed")

    def test_the_hook_marks_its_liveness_id(self):
        self.assertIn("Mark(nevr_hook_liveness::kOvrGetPerfStats)", strip_comments(read("src/runtime/frame/frame_stats.cpp")))


if __name__ == "__main__":
    unittest.main()
