// Host test for the clock-level override (#928): the decision (src/quest/sentinel/clock_levels_hook.cpp), built without
// exceptions as the sentinel builds it. The override owns no GOT slot: the render-configuration hook's handler asks it which
// levels to call with, and render_config_hook_test.cpp drives that handler through the thunk entry. got_pinned_test.cpp
// resolves the pinned slot in the real library (just test-quest-hooks-pinned).
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "clock_levels_hook.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/tests/test_check.h"

namespace {

namespace cl = nevr_quest::clock_levels;

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

bool SomeLineContains(const char* needle) {
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}

int CountLinesContaining(const char* needle) {
  int n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

void Reset(bool overrideOn) {
  g_lines.clear();
  cl::ResetForTest();
  cl::ArmForTest(overrideOn);
}

void TheGamesTwoTwoIsRaisedToFourFourWhenTheSwitchIsOn() {
  Reset(true);
  const cl::Levels l = cl::Choose(2, 2);
  QCHECK(l.cpu == 4 && l.gpu == 4);
  const cl::Counts counts = cl::CurrentCounts();
  QCHECK(counts.calls == 1 && counts.overridden == 1);
}

void WithTheSwitchOffTheGamesTwoTwoIsLeftAloneAndStillCounted() {
  Reset(false);
  const cl::Levels l = cl::Choose(2, 2);
  QCHECK(l.cpu == 2 && l.gpu == 2);
  const cl::Counts counts = cl::CurrentCounts();
  QCHECK(counts.calls == 1 && counts.overridden == 0);
}

void AnyOtherPairPassesThroughUnchanged() {
  struct Pair { std::int32_t cpu, gpu; };
  // (5, 5) is what the game asks for when CGRenderConfig::StablePowerState() is true; a pair with only one level at the
  // game's value is not the game's request either.
  for (const Pair p : {Pair{3, 1}, Pair{5, 5}, Pair{2, 3}, Pair{3, 2}, Pair{0, 0}, Pair{4, 4}, Pair{1, 2}, Pair{2, 1}}) {
    Reset(true);
    const cl::Levels l = cl::Choose(p.cpu, p.gpu);
    QCHECK(l.cpu == p.cpu && l.gpu == p.gpu);
    const cl::Counts counts = cl::CurrentCounts();
    QCHECK(counts.calls == 1 && counts.overridden == 0);
  }
}

void EveryCallIsCountedAndEveryRewriteToo() {
  Reset(true);
  for (int i = 0; i < 5; ++i) cl::Choose(2, 2);
  cl::Choose(5, 5);
  const cl::Counts counts = cl::CurrentCounts();
  QCHECK(counts.calls == 6 && counts.overridden == 5);
}

void TheLevelsAreTheOnesTheOverrideIsDefinedFor() {
  QCHECK(cl::kGameCpuLevel == 2 && cl::kGameGpuLevel == 2);
  QCHECK(cl::kOverrideCpuLevel == 4 && cl::kOverrideGpuLevel == 4);
}

// Arming says the switch once, whichever way it is set, and sets it.
void ArmingSaysTheSwitchEitherWayAndSetsIt() {
  for (const bool overrideOn : {true, false}) {
    Reset(!overrideOn);
    const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
    const bool ok = cl::Arm(overrideOn);
    sentinel::SetLogSink(previous);
    QCHECK(ok);
    QCHECK(CountLinesContaining("\"event\":\"clock_levels_arm\"") == 1);
    QCHECK(SomeLineContains("\"result\":\"armed\""));
    QCHECK(SomeLineContains("\"level\":\"info\""));
    QCHECK(SomeLineContains(overrideOn ? "\"override\":\"on\"" : "\"override\":\"off\""));
    const cl::Levels l = cl::Choose(2, 2);
    QCHECK(l.cpu == (overrideOn ? 4 : 2) && l.gpu == (overrideOn ? 4 : 2));
  }
}

// Without its counters the override stays off, says so at Error, and rewrites nothing, whatever the switch asked.
void WithoutItsCountersTheSwitchStaysOffAndSaysSo() {
  Reset(false);
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const bool ok = cl::ArmIfCounted(false, true);
  sentinel::SetLogSink(previous);
  QCHECK(!ok);
  QCHECK(g_lines.size() == 1);
  QCHECK(SomeLineContains("\"event\":\"clock_levels_arm\""));
  QCHECK(SomeLineContains("\"result\":\"skipped\""));
  QCHECK(SomeLineContains("\"why\":\"counters_refused\""));
  QCHECK(SomeLineContains("\"level\":\"error\""));
  QCHECK(SomeLineContains("\"override\":\"on\""));  // what was asked for
  const cl::Levels l = cl::Choose(2, 2);
  QCHECK(l.cpu == 2 && l.gpu == 2);  // what happened
}

void WithItsCountersTheSwitchIsSet() {
  Reset(false);
  QCHECK(cl::ArmIfCounted(true, true));
  const cl::Levels l = cl::Choose(2, 2);
  QCHECK(l.cpu == 4 && l.gpu == 4);
}

void TheTwoCountersRegisterAndLeaveTheRestOfTheTable() {
  sentinel::StopReporter();
  QCHECK(cl::RegisterCounters());
  int extra = 0;
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  while (extra < static_cast<int>(sentinel::kMaxReportCounters) &&
         sentinel::RegisterReportCounter("spare", &spare[extra])) {
    ++extra;
  }
  QCHECK(extra == static_cast<int>(sentinel::kMaxReportCounters) - cl::kCounterCount);
  sentinel::StopReporter();
}

// A table with room for fewer than two more: the registration is refused as a whole, said once at Error with the counts, and
// the caller is told, so the ctor sequence arms nothing without its counters.
void ARefusedCounterIsReportedAndFailsTheRegistration() {
  sentinel::StopReporter();
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  int filled = 0;
  while (filled < static_cast<int>(sentinel::kMaxReportCounters) - 1 &&
         sentinel::RegisterReportCounter("spare", &spare[filled])) {
    ++filled;
  }
  QCHECK(filled == static_cast<int>(sentinel::kMaxReportCounters) - 1);
  g_lines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const bool ok = cl::RegisterCounters();
  sentinel::SetLogSink(previous);
  QCHECK(!ok);
  QCHECK(CountLinesContaining("\"event\":\"clock_levels_counters\"") == 1);
  QCHECK(SomeLineContains("\"result\":\"refused\""));
  QCHECK(SomeLineContains("\"level\":\"error\""));
  QCHECK(SomeLineContains("\"registered\":1"));
  QCHECK(SomeLineContains("\"wanted\":2"));
  sentinel::StopReporter();
}

}  // namespace

int main() {
  TheGamesTwoTwoIsRaisedToFourFourWhenTheSwitchIsOn();
  WithTheSwitchOffTheGamesTwoTwoIsLeftAloneAndStillCounted();
  AnyOtherPairPassesThroughUnchanged();
  EveryCallIsCountedAndEveryRewriteToo();
  TheLevelsAreTheOnesTheOverrideIsDefinedFor();
  ArmingSaysTheSwitchEitherWayAndSetsIt();
  WithoutItsCountersTheSwitchStaysOffAndSaysSo();
  WithItsCountersTheSwitchIsSet();
  TheTwoCountersRegisterAndLeaveTheRestOfTheTable();
  ARefusedCounterIsReportedAndFailsTheRegistration();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "clock_levels_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("clock_levels_hook_test: all checks pass\n");
  return 0;
}
