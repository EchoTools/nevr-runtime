// Host test for the clock-level hook (#928): the real handler (src/quest/sentinel/clock_levels_hook.cpp), built without
// exceptions as the sentinel builds it, driven through the thunk's entry with a stand-in for vrapi_SetClockLevels. No GOT
// slot is touched: got_pinned_test.cpp resolves the pinned slot in the real library (just test-quest-hooks-pinned), and
// tests/quest TestHookFramesCarryNoPersonality covers the frames.
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "clock_levels_hook.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/tests/test_check.h"

namespace {

namespace cl = nevr_quest::clock_levels;
using sentinel::pinned::OvrMobileOpaque;

// What VrApi saw: every call of the stand-in.
struct Seen {
  int calls = 0;
  OvrMobileOpaque* ovr = nullptr;
  std::int32_t cpu = 0;
  std::int32_t gpu = 0;
};
Seen g_seen;
std::int32_t g_answer = 0;  // ovrSuccess

std::int32_t RealSetClockLevels(OvrMobileOpaque* ovr, std::int32_t cpu, std::int32_t gpu) {
  ++g_seen.calls;
  g_seen.ovr = ovr;
  g_seen.cpu = cpu;
  g_seen.gpu = gpu;
  return g_answer;
}

cl::Thunk::Fn Entry() { return reinterpret_cast<cl::Thunk::Fn>(cl::Thunk::EntryAddress()); }

OvrMobileOpaque* const kSession = reinterpret_cast<OvrMobileOpaque*>(0x7a1234);

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

// Armed with the switch as given, the log captured, the stand-in installed as the thunk's original.
void Reset(bool overrideOn) {
  g_seen = Seen();
  g_answer = 0;
  g_lines.clear();
  cl::ResetForTest();
  cl::Thunk::Reset();
  *cl::Thunk::OriginalOut() = reinterpret_cast<void*>(&RealSetClockLevels);
  cl::ArmForTest(overrideOn);
}

// The game's call at CVR::UpdateTracking, with the log captured around it.
std::int32_t GameCall(std::int32_t cpu, std::int32_t gpu) {
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const std::int32_t result = Entry()(kSession, cpu, gpu);
  sentinel::SetLogSink(previous);
  return result;
}

void TheGamesTwoTwoIsRaisedToFourFour() {
  Reset(true);
  g_answer = 0;
  const std::int32_t result = GameCall(2, 2);
  QCHECK(g_seen.calls == 1);
  QCHECK(g_seen.cpu == 4 && g_seen.gpu == 4);
  QCHECK(g_seen.ovr == kSession);  // the session is the game's
  QCHECK(result == 0);
  const cl::Counts counts = cl::CurrentCounts();
  QCHECK(counts.calls == 1 && counts.overridden == 1);
  QCHECK(g_lines.size() == 1);
  QCHECK(SomeLineContains("\"level\":\"info\""));
  QCHECK(SomeLineContains("\"event\":\"clock_levels\""));
  QCHECK(SomeLineContains("\"requested_cpu\":2"));
  QCHECK(SomeLineContains("\"requested_gpu\":2"));
  QCHECK(SomeLineContains("\"applied_cpu\":4"));
  QCHECK(SomeLineContains("\"applied_gpu\":4"));
  QCHECK(SomeLineContains("\"result\":0"));
  QCHECK(SomeLineContains("\"override\":\"on\""));
}

void TheOriginalsResultComesBackWhateverItIs() {
  Reset(true);
  g_answer = -1000;  // an ovrResult failure
  QCHECK(GameCall(2, 2) == -1000);
  QCHECK(g_seen.cpu == 4 && g_seen.gpu == 4);
  QCHECK(SomeLineContains("\"result\":-1000"));
  Reset(true);
  g_answer = 7;
  QCHECK(GameCall(3, 1) == 7);
}

void AnyOtherPairPassesThroughUnchanged() {
  struct Pair { std::int32_t cpu, gpu; };
  // (5, 5) is what the game asks for when CGRenderConfig::StablePowerState() is true; a pair with only one level at the
  // game's value is not the game's request either.
  for (const Pair p : {Pair{3, 1}, Pair{5, 5}, Pair{2, 3}, Pair{3, 2}, Pair{0, 0}, Pair{4, 4}, Pair{1, 2}, Pair{2, 1}}) {
    Reset(true);
    GameCall(p.cpu, p.gpu);
    QCHECK(g_seen.calls == 1);
    QCHECK(g_seen.cpu == p.cpu && g_seen.gpu == p.gpu);
    const cl::Counts counts = cl::CurrentCounts();
    QCHECK(counts.calls == 1 && counts.overridden == 0);
    QCHECK(g_lines.size() == 1);
    const std::string requested = "\"requested_cpu\":" + std::to_string(p.cpu) + ",\"requested_gpu\":" + std::to_string(p.gpu);
    const std::string applied = "\"applied_cpu\":" + std::to_string(p.cpu) + ",\"applied_gpu\":" + std::to_string(p.gpu);
    QCHECK(SomeLineContains(requested.c_str()));
    QCHECK(SomeLineContains(applied.c_str()));
    QCHECK(SomeLineContains("\"override\":\"on\""));  // the switch, not whether this call was rewritten
  }
}

void WithTheSwitchOffTheGamesTwoTwoPassesThroughAndIsStillLogged() {
  Reset(false);
  const std::int32_t result = GameCall(2, 2);
  QCHECK(result == 0);
  QCHECK(g_seen.calls == 1);
  QCHECK(g_seen.cpu == 2 && g_seen.gpu == 2);
  const cl::Counts counts = cl::CurrentCounts();
  QCHECK(counts.calls == 1 && counts.overridden == 0);
  QCHECK(g_lines.size() == 1);
  QCHECK(SomeLineContains("\"requested_cpu\":2"));
  QCHECK(SomeLineContains("\"applied_cpu\":2"));
  QCHECK(SomeLineContains("\"applied_gpu\":2"));
  QCHECK(SomeLineContains("\"override\":\"off\""));
}

void ADisarmedHookPassesEverythingThroughWithoutALine() {
  Reset(true);
  cl::Thunk::Disarm();
  GameCall(2, 2);
  QCHECK(g_seen.calls == 1);
  QCHECK(g_seen.cpu == 2 && g_seen.gpu == 2);
  QCHECK(g_lines.empty());
  QCHECK(cl::CurrentCounts().calls == 0);
}

void AThunkWithoutAnOriginalIsAFaultAndRewritesNothing() {
  Reset(true);
  *cl::Thunk::OriginalOut() = nullptr;
  QCHECK(GameCall(2, 2) == 0);
  QCHECK(g_seen.calls == 0);
  QCHECK(cl::Thunk::Faults() == 1);
  QCHECK(cl::CurrentCounts().overridden == 0);
  QCHECK(g_lines.empty());
}

// The game calls once per entry into VR mode; a log that cannot grow without bound is cheap insurance for a call that turns
// out to be more frequent. Calls past the cap are handled the same and counted.
void TheLogIsBoundedAndTheCountIsNot() {
  Reset(true);
  const unsigned total = cl::kMaxLoggedCalls + 6;
  for (unsigned i = 0; i < total; ++i) GameCall(2, 2);
  QCHECK(g_lines.size() == cl::kMaxLoggedCalls);
  const cl::Counts counts = cl::CurrentCounts();
  QCHECK(counts.calls == total && counts.overridden == total);
  QCHECK(g_seen.calls == static_cast<int>(total));
  QCHECK(g_seen.cpu == 4 && g_seen.gpu == 4);  // calls past the cap are still rewritten
}

// Install with no libr15 in this process: refused, said once with the switch, and the original call stays the game's.
void InstallWithoutTheModuleIsRefusedAndSaysTheSwitchEitherWay() {
  for (const bool overrideOn : {true, false}) {
    Reset(!overrideOn);
    const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
    const bool ok = cl::Install(overrideOn);
    sentinel::SetLogSink(previous);
    QCHECK(!ok);
    // GotHook writes its own refusal line as well; ours is the one that carries the switch.
    QCHECK(CountLinesContaining("\"event\":\"clock_levels_install\"") == 1);
    QCHECK(SomeLineContains("\"result\":\"failed\""));
    QCHECK(SomeLineContains("\"level\":\"error\""));
    QCHECK(SomeLineContains(overrideOn ? "\"override\":\"on\"" : "\"override\":\"off\""));
    QCHECK(SomeLineContains("\"slot\":\"module_not_loaded\""));
    // Install stored the switch it was given, whatever the slot did: the next call is rewritten only when it is on.
    g_seen = Seen();
    GameCall(2, 2);
    QCHECK(g_seen.calls == 1);
    QCHECK(g_seen.cpu == (overrideOn ? 4 : 2) && g_seen.gpu == (overrideOn ? 4 : 2));
  }
}

void WithoutItsCountersTheHookInstallsNothingAndSaysSo() {
  Reset(false);
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const bool ok = cl::InstallIfCounted(false, true);
  sentinel::SetLogSink(previous);
  QCHECK(!ok);
  QCHECK(g_lines.size() == 1);
  QCHECK(SomeLineContains("\"event\":\"clock_levels_install\""));
  QCHECK(SomeLineContains("\"result\":\"skipped\""));
  QCHECK(SomeLineContains("\"why\":\"counters_refused\""));
  QCHECK(SomeLineContains("\"override\":\"on\""));
  QCHECK(!SomeLineContains("\"slot\""));  // GotHook was never asked
}

// The pin: libr15's vrapi_SetClockLevels JUMP_SLOT in the pinned image (`readelf -rW`). got_pinned_test.cpp resolves it in the
// real library; this keeps the constants from drifting from the values read off the image.
void TheSlotIsThePinnedJumpSlotOfLibR15() {
  const sentinel::GotTarget t = sentinel::pinned::LibR15SetClockLevels();
  QCHECK(std::strcmp(t.module, "libr15.so") == 0);
  QCHECK(std::strcmp(t.symbol, "vrapi_SetClockLevels") == 0);
  QCHECK(t.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(t.buildId != nullptr && std::strcmp(t.buildId, sentinel::pinned::kLibR15BuildId) == 0);
  QCHECK(t.slotVaddr.has_value() && *t.slotVaddr == 0x36c75f0ULL);
}

void TheLevelsAreTheOnesTheHookIsDefinedFor() {
  QCHECK(cl::kGameCpuLevel == 2 && cl::kGameGpuLevel == 2);
  QCHECK(cl::kOverrideCpuLevel == 4 && cl::kOverrideGpuLevel == 4);
}

void TheThreeCountersRegisterAndLeaveTheRestOfTheTable() {
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

// A table with room for fewer than three more: the registration is refused as a whole, said once at Error with the counts, and
// the caller is told, so the ctor sequence installs nothing without its counters.
void ARefusedCounterIsReportedAndFailsTheRegistration() {
  sentinel::StopReporter();
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  int filled = 0;
  while (filled < static_cast<int>(sentinel::kMaxReportCounters) - 2 &&
         sentinel::RegisterReportCounter("spare", &spare[filled])) {
    ++filled;
  }
  QCHECK(filled == static_cast<int>(sentinel::kMaxReportCounters) - 2);
  g_lines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const bool ok = cl::RegisterCounters();
  sentinel::SetLogSink(previous);
  QCHECK(!ok);
  QCHECK(CountLinesContaining("\"event\":\"clock_levels_counters\"") == 1);
  QCHECK(SomeLineContains("\"result\":\"refused\""));
  QCHECK(SomeLineContains("\"level\":\"error\""));
  QCHECK(SomeLineContains("\"registered\":2"));
  QCHECK(SomeLineContains("\"wanted\":3"));
  sentinel::StopReporter();
}

}  // namespace

int main() {
  TheGamesTwoTwoIsRaisedToFourFour();
  TheOriginalsResultComesBackWhateverItIs();
  AnyOtherPairPassesThroughUnchanged();
  WithTheSwitchOffTheGamesTwoTwoPassesThroughAndIsStillLogged();
  ADisarmedHookPassesEverythingThroughWithoutALine();
  AThunkWithoutAnOriginalIsAFaultAndRewritesNothing();
  TheLogIsBoundedAndTheCountIsNot();
  InstallWithoutTheModuleIsRefusedAndSaysTheSwitchEitherWay();
  WithoutItsCountersTheHookInstallsNothingAndSaysSo();
  TheSlotIsThePinnedJumpSlotOfLibR15();
  TheLevelsAreTheOnesTheHookIsDefinedFor();
  TheThreeCountersRegisterAndLeaveTheRestOfTheTable();
  ARefusedCounterIsReportedAndFailsTheRegistration();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "clock_levels_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("clock_levels_hook_test: all checks pass\n");
  return 0;
}
