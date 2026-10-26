// Host test for the texture-quality hook (#943): the real handler (src/quest/sentinel/texture_quality_hook.cpp), built
// without exceptions as the sentinel builds it, driven through the thunk's entry with a stand-in for libr15's
// vrapi_GetSystemPropertyInt. No GOT slot is touched: got_pinned_test.cpp resolves the pinned slot in the real library,
// and tests/quest TestHookFramesCarryNoPersonality covers the frames.
//
// The expected sizes are written out here as literals, so a change to the table in texture_quality.h fails this test
// rather than following it.
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "hook_report.h"
#include "quest/tests/test_check.h"
#include "sysprop_int_share.h"
#include "texture_quality_hook.h"

namespace {

namespace tq = nevr_quest::texture_quality;
using sentinel::pinned::OvrJavaOpaque;

// The runtime's own answers, synthetic: this test says nothing about any headset's native size.
constexpr int kNativeWidth = 2064;
constexpr int kNativeHeight = 2208;

int g_realCalls = 0;
const OvrJavaOpaque* g_lastJava = nullptr;
int g_lastId = -1;
int NativeAnswer(int id) {
  if (id == 5) return kNativeWidth;
  if (id == 6) return kNativeHeight;
  return id * 10 + 7;
}
int FakeRuntime(const OvrJavaOpaque* java, int id) {
  ++g_realCalls;
  g_lastJava = java;
  g_lastId = id;
  return NativeAnswer(id);
}

tq::SystemPropertyIntThunk::Fn Entry() {
  return reinterpret_cast<tq::SystemPropertyIntThunk::Fn>(tq::SystemPropertyIntThunk::EntryAddress());
}

const OvrJavaOpaque* const kJava = reinterpret_cast<const OvrJavaOpaque*>(0x5eed);

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

struct Capture {
  sentinel::LogSink previous;
  Capture() : previous(sentinel::SetLogSink(&CaptureLine)) { g_lines.clear(); }
  ~Capture() { sentinel::SetLogSink(previous); }
};

std::size_t CountLines(const char* needle) {
  std::size_t n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1U : 0U;
  return n;
}

// The text after `"key":` up to the closing quote (a string) or the next ',' or '}' (a number).
std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t from = at + needle.size();
  if (from < line.size() && line[from] == '"') {
    const std::size_t end = line.find('"', from + 1);
    return line.substr(from + 1, end == std::string::npos ? std::string::npos : end - from - 1);
  }
  const std::size_t end = line.find_first_of(",}", from);
  return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

// The one "texture_quality" line, or "" when there is not exactly one.
std::string TheLine() {
  std::string found;
  int n = 0;
  for (const std::string& l : g_lines) {
    if (l.find("\"event\":\"texture_quality\"") != std::string::npos) {
      found = l;
      ++n;
    }
  }
  return n == 1 ? found : std::string();
}

void Reset() {
  g_realCalls = 0;
  g_lastJava = nullptr;
  g_lastId = -1;
  tq::ResetForTest();
  tq::SystemPropertyIntThunk::Reset();
  *tq::SystemPropertyIntThunk::OriginalOut() = reinterpret_cast<void*>(&FakeRuntime);
  sentinel::SetSysPropIntObserver(nullptr);
}

struct Row {
  tq::Level level;
  const char* name;
  int width;
  int height;
};
// The installer's table, per eye (the community patch repository's Graphics Patch setting; see texture_quality.h).
const Row kRows[] = {
    {tq::Level::kLow, "low", 1680, 1760},
    {tq::Level::kMedium, "medium", 2376, 2568},
    {tq::Level::kHigh, "high", 3072, 3380},
    {tq::Level::kVeryHigh, "very_high", 3584, 3944},
    {tq::Level::kUltra, "ultra", 4096, 4506},
};

// ---- the table and the words --------------------------------------------------------------------------------------------

void TheTableIsTheInstallersTable() {
  for (const Row& r : kRows) {
    QCHECK(tq::SizeFor(r.level).width == r.width);
    QCHECK(tq::SizeFor(r.level).height == r.height);
    QCHECK(std::strcmp(tq::LevelName(r.level), r.name) == 0);
  }
  QCHECK(tq::SizeFor(tq::Level::kStock).width == 0 && tq::SizeFor(tq::Level::kStock).height == 0);
  QCHECK(std::strcmp(tq::LevelName(tq::Level::kStock), "stock") == 0);
  // The pin: the flip to `high` is this line and the constant, after a Quest 2 is measured at `high` (#943). A mistake is
  // `stock` whatever the default is.
  QCHECK(tq::kDefaultLevel == tq::Level::kStock);
  QCHECK(tq::kFallbackLevel == tq::Level::kStock);
  QCHECK(tq::kEyeWidthPropertyId == 5 && tq::kEyeHeightPropertyId == 6);
}

void OnlyTheSixWordsParse() {
  for (const Row& r : kRows) {
    tq::Level out = tq::Level::kStock;
    QCHECK(tq::ParseLevel(r.name, &out) && out == r.level);
  }
  tq::Level out = tq::Level::kUltra;
  QCHECK(tq::ParseLevel("stock", &out) && out == tq::Level::kStock);
  out = tq::Level::kMedium;
  for (const char* bad : {"", "High", "HIGH", "very-high", "veryhigh", "high ", " high", "ultra\n", "5", "default", "stock,low"}) {
    QCHECK(!tq::ParseLevel(bad, &out));
    QCHECK(out == tq::Level::kMedium);  // a refusal leaves the caller's value alone
  }
}

// ---- the handler --------------------------------------------------------------------------------------------------------

void WithNothingArmedEveryQueryIsTheRuntimesOwn() {
  Reset();  // the thunk is not armed: the entry calls straight through
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  QCHECK(Entry()(kJava, 6) == kNativeHeight);
  QCHECK(g_realCalls == 2);
}

void EachLevelAnswersTheEyeSizeFromTheTable() {
  for (const Row& r : kRows) {
    Reset();
    QCHECK(tq::ArmForTest(r.level));
    QCHECK(Entry()(kJava, 5) == r.width);
    QCHECK(Entry()(kJava, 6) == r.height);
    QCHECK(tq::LatchedLevel() == r.level && tq::IsLatched());
  }
}

void OnlyIds5And6AreAnsweredWithTheSizeAndTheRestPassThrough() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kUltra));
  for (int id = -3; id <= 64; ++id) {
    if (id == 5 || id == 6) continue;
    g_realCalls = 0;
    g_lastId = -1;
    g_lastJava = nullptr;
    QCHECK(Entry()(kJava, id) == NativeAnswer(id));
    QCHECK(g_realCalls == 1 && g_lastId == id && g_lastJava == kJava);
  }
  QCHECK(tq::CurrentCounts().served == 0);  // none of those was an answer we replaced
}

void TheOriginalIsCalledOnceWithTheGamesArgumentsForTheTwoIdsToo() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kHigh));
  g_realCalls = 0;
  QCHECK(Entry()(kJava, 5) == 3072);
  QCHECK(g_realCalls == 1 && g_lastId == 5 && g_lastJava == kJava);
  QCHECK(Entry()(kJava, 6) == 3380);
  QCHECK(g_realCalls == 2 && g_lastId == 6 && g_lastJava == kJava);
}

void TheLevelIsLatchedAndANewOneIsRefused() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kHigh));
  QCHECK(!tq::ArmForTest(tq::Level::kLow));
  QCHECK(!tq::ArmForTest(tq::Level::kUltra));
  QCHECK(tq::ArmForTest(tq::Level::kHigh));  // the same level again is not a change
  QCHECK(tq::LatchedLevel() == tq::Level::kHigh);
  QCHECK(Entry()(kJava, 5) == 3072);
  QCHECK(Entry()(kJava, 6) == 3380);
  // Stock never changes a latched level either.
  QCHECK(!tq::ArmForTest(tq::Level::kStock));
  QCHECK(Entry()(kJava, 5) == 3072);
}

void StockArmsNothing() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kStock));
  QCHECK(!tq::IsLatched());
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  QCHECK(Entry()(kJava, 6) == kNativeHeight);
}

void ADisarmedHookPassesEverythingThrough() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kHigh));
  tq::SystemPropertyIntThunk::Disarm();
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  QCHECK(Entry()(kJava, 6) == kNativeHeight);
}

void AThunkWithoutAnOriginalIsAFaultAndOverridesNothing() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kHigh));
  *tq::SystemPropertyIntThunk::OriginalOut() = nullptr;
  QCHECK(Entry()(kJava, 5) == 0);
  QCHECK(tq::SystemPropertyIntThunk::Faults() == 1);
  QCHECK(tq::CurrentCounts().served == 0);
}

void TheCountersCarryTheRuntimesAnswerAndWhatWeReplaced() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kHigh));
  tq::Counts c = tq::CurrentCounts();
  QCHECK(c.nativeWidth == 0 && c.nativeHeight == 0 && c.served == 0);
  QCHECK(Entry()(kJava, 5) == 3072);
  QCHECK(Entry()(kJava, 6) == 3380);
  QCHECK(Entry()(kJava, 7) == 77);
  c = tq::CurrentCounts();
  QCHECK(c.nativeWidth == static_cast<unsigned long long>(kNativeWidth));
  QCHECK(c.nativeHeight == static_cast<unsigned long long>(kNativeHeight));
  QCHECK(c.served == 2);
  QCHECK(Entry()(kJava, 5) == 3072);
  QCHECK(tq::CurrentCounts().served == 3);
}

// ---- Install, with the GOT step supplied ---------------------------------------------------------------------------------

sentinel::GotStatus GotAccepts() noexcept { return sentinel::GotStatus::kOk; }
sentinel::GotStatus GotRefuses() noexcept { return sentinel::GotStatus::kBuildIdMismatch; }
int g_gotCalls = 0;
sentinel::GotStatus GotCounts() noexcept {
  ++g_gotCalls;
  return sentinel::GotStatus::kOk;
}

void AnInstalledLevelLogsOneLineWithTheLevelTheSourceAndTheSize() {
  for (const bool fromConfig : {true, false}) {
    for (const Row& r : kRows) {
      Reset();
      Capture log;
      QCHECK(tq::InstallVia(r.level, fromConfig, &GotAccepts));
      const std::string line = TheLine();
      QCHECK(!line.empty());
      QCHECK(Field(line, "quality") == r.name);
      QCHECK(Field(line, "source") == (fromConfig ? "config" : "default"));
      QCHECK(Field(line, "w") == std::to_string(r.width));
      QCHECK(Field(line, "h") == std::to_string(r.height));
      QCHECK(Field(line, "result") == "installed");
      QCHECK(line.find("\"level\":\"info\"") != std::string::npos);  // the severity key, not the setting
      QCHECK(g_lines.size() == 1);
      QCHECK(tq::IsLatched() && tq::LatchedLevel() == r.level);
      QCHECK(sentinel::SysPropIntSlotOwned());
      QCHECK(Entry()(kJava, 5) == r.width && Entry()(kJava, 6) == r.height);
    }
  }
}

void StockInstallsNothingAndSaysSo() {
  for (const bool fromConfig : {true, false}) {
    Reset();
    Capture log;
    g_gotCalls = 0;
    QCHECK(tq::InstallVia(tq::Level::kStock, fromConfig, &GotCounts));
    QCHECK(g_gotCalls == 0);  // no GOT step at all
    const std::string line = TheLine();
    QCHECK(Field(line, "result") == "not_installed");
    QCHECK(Field(line, "quality") == "stock");
    QCHECK(Field(line, "source") == (fromConfig ? "config" : "default"));
    QCHECK(Field(line, "w") == "0" && Field(line, "h") == "0");
    QCHECK(g_lines.size() == 1);
    QCHECK(!tq::IsLatched());
    QCHECK(!sentinel::SysPropIntSlotOwned());
    QCHECK(Entry()(kJava, 5) == kNativeWidth);  // nothing is armed
    QCHECK(Entry()(kJava, 6) == kNativeHeight);
  }
  // The real Install of `stock` needs no libr15 and no counters either.
  Reset();
  Capture log;
  QCHECK(tq::Install(tq::Level::kStock, true));
  QCHECK(Field(TheLine(), "result") == "not_installed");
  Reset();
  g_lines.clear();
  QCHECK(tq::InstallIfCounted(tq::Level::kStock, false, false));
  QCHECK(Field(TheLine(), "result") == "not_installed");
}

void ARefusedSlotLeavesNoOverrideAndIsLoud() {
  Reset();
  Capture log;
  QCHECK(!tq::InstallVia(tq::Level::kHigh, false, &GotRefuses));
  const std::string line = TheLine();
  QCHECK(Field(line, "result") == "refused");
  QCHECK(Field(line, "status") == sentinel::GotStatusName(sentinel::GotStatus::kBuildIdMismatch));
  QCHECK(Field(line, "quality") == "high" && Field(line, "source") == "default");
  QCHECK(Field(line, "w") == "0" && Field(line, "h") == "0");  // the game gets the runtime's sizes
  QCHECK(line.find("\"level\":\"error\"") != std::string::npos);
  QCHECK(!tq::IsLatched());
  QCHECK(!sentinel::SysPropIntSlotOwned());
  // Were the thunk ever entered, it must not override.
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  QCHECK(Entry()(kJava, 6) == kNativeHeight);
}

void TheRealInstallIsRefusedOnAHostWithoutLibr15AndOverridesNothing() {
  Reset();
  Capture log;
  QCHECK(!tq::Install(tq::Level::kHigh, true));
  QCHECK(Field(TheLine(), "result") == "refused");
  QCHECK(!tq::IsLatched());
  QCHECK(!sentinel::SysPropIntSlotOwned());
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
}

void ASecondLevelIsRefusedAndTheFirstKeepsAnswering() {
  Reset();
  Capture log;
  QCHECK(tq::InstallVia(tq::Level::kHigh, true, &GotAccepts));
  g_lines.clear();
  g_gotCalls = 0;
  QCHECK(!tq::InstallVia(tq::Level::kUltra, true, &GotCounts));
  QCHECK(g_gotCalls == 0);  // never reaches the slot
  const std::string line = TheLine();
  QCHECK(Field(line, "result") == "refused");
  QCHECK(Field(line, "why") == "level_already_latched");
  QCHECK(Field(line, "latched") == "high");
  QCHECK(Field(line, "quality") == "ultra");
  QCHECK(tq::LatchedLevel() == tq::Level::kHigh);
  QCHECK(sentinel::SysPropIntSlotOwned());  // the first install still owns the slot
  QCHECK(Entry()(kJava, 5) == 3072 && Entry()(kJava, 6) == 3380);
}

void WithoutItsCountersTheHookIsNotInstalledAndSaysWhy() {
  Reset();
  Capture log;
  g_gotCalls = 0;
  QCHECK(!tq::InstallIfCounted(tq::Level::kHigh, false, false));
  const std::string line = TheLine();
  QCHECK(Field(line, "result") == "skipped");
  QCHECK(Field(line, "why") == "counters_refused");
  QCHECK(Field(line, "quality") == "high" && Field(line, "source") == "default");
  QCHECK(Field(line, "w") == "0" && Field(line, "h") == "0");
  QCHECK(CountLines("got_hook") == 0);
  QCHECK(!tq::IsLatched());
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
}

// ---- the shared slot -----------------------------------------------------------------------------------------------------

struct Seen {
  int calls = 0;
  const void* java = nullptr;
  int id = -1;
  int result = -1;
};
Seen g_seen;
void Observer(const void* java, int id, int result) noexcept {
  ++g_seen.calls;
  g_seen.java = java;
  g_seen.id = id;
  g_seen.result = result;
}

void TheObserverIsToldTheRuntimesAnswerNotTheOverride() {
  Reset();
  g_seen = Seen();
  QCHECK(sentinel::GetSysPropIntObserver() == nullptr);
  sentinel::SetSysPropIntObserver(&Observer);
  QCHECK(tq::ArmForTest(tq::Level::kHigh));
  QCHECK(Entry()(kJava, 5) == 3072);
  QCHECK(g_seen.calls == 1 && g_seen.id == 5 && g_seen.java == kJava && g_seen.result == kNativeWidth);
  QCHECK(Entry()(kJava, 6) == 3380);
  QCHECK(g_seen.calls == 2 && g_seen.id == 6 && g_seen.result == kNativeHeight);
  QCHECK(Entry()(kJava, 9) == 97);
  QCHECK(g_seen.calls == 3 && g_seen.id == 9 && g_seen.result == 97);
  sentinel::SetSysPropIntObserver(nullptr);
  QCHECK(Entry()(kJava, 9) == 97);
  QCHECK(g_seen.calls == 3);
}

void TheObserverSeesStockQueriesToo() {
  Reset();
  g_seen = Seen();
  sentinel::SetSysPropIntObserver(&Observer);
  QCHECK(tq::ArmForTest(tq::Level::kStock));
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  QCHECK(g_seen.calls == 1 && g_seen.result == kNativeWidth);
  sentinel::SetSysPropIntObserver(nullptr);
}

// ---- the counters --------------------------------------------------------------------------------------------------------

void TheFourCountersRegisterAndLeaveTheRestOfTheTable() {
  sentinel::StopReporter();
  Capture log;
  QCHECK(tq::RegisterCounters());
  QCHECK(CountLines("texture_quality_counters") == 0);
  int extra = 0;
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  while (extra < static_cast<int>(sentinel::kMaxReportCounters) &&
         sentinel::RegisterReportCounter("spare", &spare[extra])) {
    ++extra;
  }
  QCHECK(extra == static_cast<int>(sentinel::kMaxReportCounters) - tq::kCounterCount);
  sentinel::StopReporter();
}

void ARefusedCounterIsOneLoudLine() {
  sentinel::StopReporter();
  constexpr unsigned kFill = sentinel::kMaxReportCounters - 2;
  static std::atomic<std::uint64_t> filler[kFill];
  unsigned taken = 0;
  for (unsigned i = 0; i < kFill; ++i) taken += sentinel::RegisterReportCounter("filler", &filler[i]) ? 1U : 0U;
  QCHECK(taken == kFill);
  Capture log;
  QCHECK(!tq::RegisterCounters());
  QCHECK(CountLines("\"event\":\"texture_quality_counters\"") == 1);
  QCHECK(CountLines("\"result\":\"refused\"") == 1);
  bool errorLine = false;  // the table's own refusals are error lines too; this event's line must be one
  for (const std::string& l : g_lines) {
    if (l.find("\"event\":\"texture_quality_counters\"") != std::string::npos &&
        l.find("\"level\":\"error\"") != std::string::npos) {
      errorLine = true;
    }
  }
  QCHECK(errorLine);
  sentinel::StopReporter();
}

}  // namespace

int main() {
  TheTableIsTheInstallersTable();
  OnlyTheSixWordsParse();
  WithNothingArmedEveryQueryIsTheRuntimesOwn();
  EachLevelAnswersTheEyeSizeFromTheTable();
  OnlyIds5And6AreAnsweredWithTheSizeAndTheRestPassThrough();
  TheOriginalIsCalledOnceWithTheGamesArgumentsForTheTwoIdsToo();
  TheLevelIsLatchedAndANewOneIsRefused();
  StockArmsNothing();
  ADisarmedHookPassesEverythingThrough();
  AThunkWithoutAnOriginalIsAFaultAndOverridesNothing();
  TheCountersCarryTheRuntimesAnswerAndWhatWeReplaced();
  AnInstalledLevelLogsOneLineWithTheLevelTheSourceAndTheSize();
  StockInstallsNothingAndSaysSo();
  ARefusedSlotLeavesNoOverrideAndIsLoud();
  TheRealInstallIsRefusedOnAHostWithoutLibr15AndOverridesNothing();
  ASecondLevelIsRefusedAndTheFirstKeepsAnswering();
  WithoutItsCountersTheHookIsNotInstalledAndSaysWhy();
  TheObserverIsToldTheRuntimesAnswerNotTheOverride();
  TheObserverSeesStockQueriesToo();
  TheFourCountersRegisterAndLeaveTheRestOfTheTable();
  ARefusedCounterIsOneLoudLine();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "texture_quality_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("texture_quality_hook_test: all checks pass\n");
  return 0;
}
