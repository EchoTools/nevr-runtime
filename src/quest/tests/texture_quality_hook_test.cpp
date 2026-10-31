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

// The runtime's own answers. The defaults are synthetic and name a Quest 3 (width at least 1680); SetStock names another
// headset's stock size. This test says nothing about any headset's native size.
constexpr int kNativeWidth = 2064;
constexpr int kNativeHeight = 2208;
int g_natW = kNativeWidth;
int g_natH = kNativeHeight;

int g_realCalls = 0;
const OvrJavaOpaque* g_lastJava = nullptr;
int g_lastId = -1;
int NativeAnswer(int id) {
  if (id == 5) return g_natW;
  if (id == 6) return g_natH;
  return id * 10 + 7;
}
void SetStock(int width, int height) {
  g_natW = width;
  g_natH = height;
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
  SetStock(kNativeWidth, kNativeHeight);
  tq::ResetForTest();
  tq::SystemPropertyIntThunk::Reset();
  *tq::SystemPropertyIntThunk::OriginalOut() = reinterpret_cast<void*>(&FakeRuntime);
  sentinel::SetSysPropIntObserver(nullptr);
}

// One level's eye size on each headset: the Quest 3 pair and the Quest 2 pair, from the installer's tables (texture_quality.h).
struct Row {
  tq::Level level;
  const char* name;
  int q3Width, q3Height;
  int q2Width, q2Height;
};
const Row kRows[] = {
    {tq::Level::kVeryLow, "very_low", 1344, 1408, 1152, 1264},
    {tq::Level::kLow, "low", 1680, 1760, 1440, 1584},
    {tq::Level::kMedium, "medium", 2376, 2568, 1728, 1904},
    {tq::Level::kHigh, "high", 3072, 3380, 2016, 2224},
    {tq::Level::kVeryHigh, "very_high", 3584, 3944, 2304, 2528},
    {tq::Level::kUltra, "ultra", 4096, 4506, 2592, 2848},
};
// The stock sizes that name each headset: a Quest 3 is 1680x1760 and a Quest 2 is 1440x1584 (texture_quality.h).
constexpr int kQ3StockW = 1680, kQ3StockH = 1760;
constexpr int kQ2StockW = 1440, kQ2StockH = 1584;

// ---- the table and the words --------------------------------------------------------------------------------------------

void TheTableIsTheInstallersTable() {
  for (const Row& r : kRows) {
    QCHECK(tq::SizeFor(r.level, tq::Headset::kQuest3).width == r.q3Width);
    QCHECK(tq::SizeFor(r.level, tq::Headset::kQuest3).height == r.q3Height);
    QCHECK(tq::SizeFor(r.level, tq::Headset::kQuest2).width == r.q2Width);
    QCHECK(tq::SizeFor(r.level, tq::Headset::kQuest2).height == r.q2Height);
    QCHECK(std::strcmp(tq::LevelName(r.level), r.name) == 0);
  }
  for (const tq::Headset h : {tq::Headset::kQuest2, tq::Headset::kQuest3}) {
    QCHECK(tq::SizeFor(tq::Level::kStock, h).width == 0 && tq::SizeFor(tq::Level::kStock, h).height == 0);
  }
  QCHECK(std::strcmp(tq::LevelName(tq::Level::kStock), "stock") == 0);
  // The pin: the flip to `high` is this line and the constant, after a Quest 2 is measured at `high` (#943). A mistake is
  // `stock` whatever the default is.
  QCHECK(tq::kDefaultLevel == tq::Level::kStock);
  QCHECK(tq::kFallbackLevel == tq::Level::kStock);
  QCHECK(tq::kEyeWidthPropertyId == 5 && tq::kEyeHeightPropertyId == 6);
}

// The split is one number on both axes: a stock size of 1680 or more is a Quest 3, anything less (and 0, unanswered) a Quest 2.
void TheHeadsetSplitsAtOneSixEightyOnBothAxes() {
  QCHECK(tq::HeadsetForStockSize(0) == tq::Headset::kQuest2);
  QCHECK(tq::HeadsetForStockSize(kQ2StockW) == tq::Headset::kQuest2);
  QCHECK(tq::HeadsetForStockSize(kQ2StockH) == tq::Headset::kQuest2);
  QCHECK(tq::HeadsetForStockSize(1679) == tq::Headset::kQuest2);
  QCHECK(tq::HeadsetForStockSize(1680) == tq::Headset::kQuest3);
  QCHECK(tq::HeadsetForStockSize(kQ3StockW) == tq::Headset::kQuest3);
  QCHECK(tq::HeadsetForStockSize(kQ3StockH) == tq::Headset::kQuest3);
}

void OnlySevenWordsParse() {
  for (const Row& r : kRows) {
    tq::Level out = tq::Level::kStock;
    QCHECK(tq::ParseLevel(r.name, &out) && out == r.level);
  }
  tq::Level out = tq::Level::kUltra;
  QCHECK(tq::ParseLevel("stock", &out) && out == tq::Level::kStock);
  out = tq::Level::kMedium;
  for (const char* bad : {"", "High", "HIGH", "very-high", "veryhigh", "very low", "high ", " high", "ultra\n", "5", "default",
                          "stock,low"}) {
    QCHECK(!tq::ParseLevel(bad, &out));
    QCHECK(out == tq::Level::kMedium);  // a refusal leaves the caller's value alone
  }
}

// ---- the sizes follow the headset ---------------------------------------------------------------------------------------

// On a Quest 2 each level answers the Quest 2 pair, and on a Quest 3 the Quest 3 pair, decided by the runtime's own
// answer to the same call.
void EachLevelAnswersTheTableOfTheHeadsetTheRuntimeNames() {
  for (const Row& r : kRows) {
    for (const bool quest3 : {false, true}) {
      Reset();
      SetStock(quest3 ? kQ3StockW : kQ2StockW, quest3 ? kQ3StockH : kQ2StockH);
      QCHECK(tq::ArmForTest(r.level));
      QCHECK(Entry()(kJava, 5) == (quest3 ? r.q3Width : r.q2Width));
      QCHECK(Entry()(kJava, 6) == (quest3 ? r.q3Height : r.q2Height));
      QCHECK(tq::CurrentCounts().served == 2);
    }
  }
}

// Low is the stock size on each headset, so it answers what the runtime answers (the native sizes, not a fixed table).
void LowAnswersTheStockSizeOfEachHeadset() {
  for (const bool quest3 : {false, true}) {
    Reset();
    SetStock(quest3 ? kQ3StockW : kQ2StockW, quest3 ? kQ3StockH : kQ2StockH);
    QCHECK(tq::ArmForTest(tq::Level::kLow));
    QCHECK(Entry()(kJava, 5) == (quest3 ? kQ3StockW : kQ2StockW));
    QCHECK(Entry()(kJava, 6) == (quest3 ? kQ3StockH : kQ2StockH));
  }
}

// ---- the handler --------------------------------------------------------------------------------------------------------

void WithNothingArmedEveryQueryIsTheRuntimesOwn() {
  Reset();  // the thunk is not armed: the entry calls straight through
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  QCHECK(Entry()(kJava, 6) == kNativeHeight);
  QCHECK(g_realCalls == 2);
}

void EachLevelLatchesAndAnswers() {
  for (const Row& r : kRows) {
    Reset();  // the default natives are a Quest 3's: the Quest 3 pair
    QCHECK(tq::ArmForTest(r.level));
    QCHECK(Entry()(kJava, 5) == r.q3Width);
    QCHECK(Entry()(kJava, 6) == r.q3Height);
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

// `stock` is a level like the others to the hook: latched, armed, and answering the runtime's own size (the settings menu can
// raise it while the game runs, #1012).
void StockAnswersTheRuntimesOwnSize() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kStock));
  QCHECK(tq::IsLatched() && tq::LatchedLevel() == tq::Level::kStock);
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  QCHECK(Entry()(kJava, 6) == kNativeHeight);
  QCHECK(tq::CurrentCounts().served == 0);
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

// ---- the level while the game runs (#1012) ------------------------------------------------------------------------------

// A request changes no answer. Only CommitApplied does, and it changes the next answer: the sizes follow the applied level.
void ARequestChangesNothingAndACommitChangesTheNextAnswer() {
  Reset();
  QCHECK(tq::RequestedLevelValue() == tq::kLevelUnset);
  QCHECK(tq::ArmForTest(tq::Level::kStock));
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kHigh));
  QCHECK(tq::LastRequestSource() == tq::RequestSource::kMenu);
  QCHECK(tq::LatchedLevel() == tq::Level::kStock);
  QCHECK(Entry()(kJava, 5) == kNativeWidth && Entry()(kJava, 6) == kNativeHeight);
  tq::CommitApplied(tq::Level::kHigh);
  QCHECK(Entry()(kJava, 5) == 3072 && Entry()(kJava, 6) == 3380);
  tq::RequestLevel(tq::Level::kVeryLow, tq::RequestSource::kProperty);
  QCHECK(tq::LastRequestSource() == tq::RequestSource::kProperty);
  QCHECK(Entry()(kJava, 5) == 3072);  // still the applied one
  tq::CommitApplied(tq::Level::kVeryLow);
  QCHECK(Entry()(kJava, 5) == 1344 && Entry()(kJava, 6) == 1408);
  tq::CommitApplied(tq::Level::kStock);
  QCHECK(Entry()(kJava, 5) == kNativeWidth);
  // On a Quest 2 the same commit gives the Quest 2 table.
  SetStock(kQ2StockW, kQ2StockH);
  tq::CommitApplied(tq::Level::kHigh);
  QCHECK(Entry()(kJava, 5) == 2016 && Entry()(kJava, 6) == 2224);
}

// The live apply compares the count across a RefreshRTSize call: only ids 5 and 6 are eye-size queries.
void OnlyTheTwoEyeSizeIdsAreCounted() {
  Reset();
  QCHECK(tq::ArmForTest(tq::Level::kLow));
  QCHECK(tq::SizeQueries() == 0);
  Entry()(kJava, 5);
  Entry()(kJava, 6);
  Entry()(kJava, 7);
  Entry()(kJava, 4);
  QCHECK(tq::SizeQueries() == 2);
}

int g_resolverCalls = 0;
int g_resolverAnswer = tq::kLevelUnset;
int Resolver() noexcept {
  ++g_resolverCalls;
  return g_resolverAnswer;
}

// The first eye-size query asks the start resolver once (the game has not delivered its settings yet), and what the player saved
// replaces the configuration's level. The second query does not ask again, and a resolver with nothing to say changes nothing.
void TheStartResolverIsAskedOnceByTheFirstQuery() {
  Reset();
  g_resolverCalls = 0;
  g_resolverAnswer = static_cast<int>(tq::Level::kHigh);
  QCHECK(tq::ArmForTest(tq::Level::kLow));
  tq::SetStartResolver(&Resolver);
  QCHECK(g_resolverCalls == 0);  // not asked at install
  QCHECK(Entry()(kJava, 5) == 3072);  // the saved level, on the very first answer
  QCHECK(Entry()(kJava, 6) == 3380);
  QCHECK(g_resolverCalls == 1);
  QCHECK(tq::LatchedLevel() == tq::Level::kHigh);
  g_resolverAnswer = static_cast<int>(tq::Level::kUltra);
  QCHECK(Entry()(kJava, 5) == 3072);
  QCHECK(g_resolverCalls == 1);

  Reset();
  g_resolverCalls = 0;
  g_resolverAnswer = tq::kLevelUnset;  // nothing saved: the configuration's level stands
  QCHECK(tq::ArmForTest(tq::Level::kMedium));
  tq::SetStartResolver(&Resolver);
  QCHECK(Entry()(kJava, 5) == 2376 && Entry()(kJava, 6) == 2568);
  QCHECK(g_resolverCalls == 1 && tq::LatchedLevel() == tq::Level::kMedium);

  // Another query kind does not ask: the resolver waits for an eye-size id.
  Reset();
  g_resolverCalls = 0;
  g_resolverAnswer = static_cast<int>(tq::Level::kHigh);
  QCHECK(tq::ArmForTest(tq::Level::kLow));
  tq::SetStartResolver(&Resolver);
  QCHECK(Entry()(kJava, 9) == 97);
  QCHECK(g_resolverCalls == 0 && tq::LatchedLevel() == tq::Level::kLow);
  QCHECK(Entry()(kJava, 5) == 3072);
  QCHECK(g_resolverCalls == 1);

  // A resolver set after the first query is never asked.
  Reset();
  g_resolverCalls = 0;
  g_resolverAnswer = static_cast<int>(tq::Level::kHigh);
  QCHECK(tq::ArmForTest(tq::Level::kLow));
  QCHECK(Entry()(kJava, 5) == 1680);
  tq::SetStartResolver(&Resolver);
  QCHECK(Entry()(kJava, 5) == 1680);
  QCHECK(g_resolverCalls == 0);
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
      QCHECK(Field(line, "w") == "0" && Field(line, "h") == "0");  // the size depends on the headset, not known at install
      QCHECK(Field(line, "sizes") == "per_headset");
      QCHECK(Field(line, "result") == "installed");
      QCHECK(line.find("\"level\":\"info\"") != std::string::npos);  // the severity key, not the setting
      QCHECK(g_lines.size() == 1);
      QCHECK(tq::IsLatched() && tq::LatchedLevel() == r.level);
      QCHECK(sentinel::SysPropIntSlotOwned());
      QCHECK(Entry()(kJava, 5) == r.q3Width && Entry()(kJava, 6) == r.q3Height);
    }
  }
}

void StockInstallsTheHookAndAnswersTheRuntimesOwnSize() {
  for (const bool fromConfig : {true, false}) {
    Reset();
    Capture log;
    g_gotCalls = 0;
    QCHECK(tq::InstallVia(tq::Level::kStock, fromConfig, &GotCounts));
    QCHECK(g_gotCalls == 1);  // the slot is taken: a level asked for later has a hook to be served by
    const std::string line = TheLine();
    QCHECK(Field(line, "result") == "installed");
    QCHECK(Field(line, "quality") == "stock");
    QCHECK(Field(line, "source") == (fromConfig ? "config" : "default"));
    QCHECK(Field(line, "w") == "0" && Field(line, "h") == "0");
    QCHECK(g_lines.size() == 1);
    QCHECK(tq::IsLatched() && tq::LatchedLevel() == tq::Level::kStock);
    QCHECK(sentinel::SysPropIntSlotOwned());
    QCHECK(Entry()(kJava, 5) == kNativeWidth);  // and it answers what the runtime answers
    QCHECK(Entry()(kJava, 6) == kNativeHeight);
    QCHECK(tq::CurrentCounts().served == 0);
  }
  // The real Install of `stock` needs libr15, like the others: on a host it is refused and overrides nothing.
  Reset();
  Capture log;
  QCHECK(!tq::Install(tq::Level::kStock, true));
  QCHECK(Field(TheLine(), "result") == "refused");
  Reset();
  g_lines.clear();
  QCHECK(!tq::InstallIfCounted(tq::Level::kStock, false, false));
  QCHECK(Field(TheLine(), "result") == "skipped");
  QCHECK(Field(TheLine(), "why") == "counters_refused");
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

// ---- the eye-size route (#1737 C4): RefreshRTSize answers through the applied level on both inputs ------------------------

std::uint64_t g_stored = 0;
const std::uint64_t g_zero = 0;
float g_scaleX = 1.0F;
int g_propCalls = 0;
int g_origRefresh = 0;

tq::EyeVector Scale(unsigned long long) noexcept { return tq::EyeVector{g_scaleX, 7.0F}; }
int Property(const OvrJavaOpaque* java, int id) noexcept {
  ++g_propCalls;
  return Entry()(java, id);
}
void FakeRefreshOriginal() { ++g_origRefresh; }

int Lo(std::uint64_t v) { return static_cast<int>(static_cast<std::int32_t>(v & 0xffffffffU)); }
int Hi(std::uint64_t v) { return static_cast<int>(static_cast<std::int32_t>(v >> 32)); }

// Resets everything and arms the route over the given pieces. A null `stored` leaves the pins unresolved.
void ArmRoute(tq::Level level, tq::EyeInput input, std::uint64_t* stored, const std::uint64_t* overridePair) {
  Reset();
  g_origRefresh = 0;
  g_propCalls = 0;
  g_scaleX = 1.0F;
  g_stored = 0;
  *tq::RefreshThunk::OriginalOut() = reinterpret_cast<void*>(&FakeRefreshOriginal);
  QCHECK(tq::ArmForTest(level));
  tq::SetInputForTest(input);
  tq::SetGameForTest(tq::EyeGame{stored, overridePair, kJava, &Scale, &Property});
  tq::ArmRefreshForTest();
}

void RefreshAnswersTheLevelsSizeOnBothHeadsets() {
  for (const bool quest3 : {true, false}) {
    for (const Row& r : kRows) {
      ArmRoute(r.level, tq::EyeInput::kReference, &g_stored, &g_zero);
      SetStock(quest3 ? kQ3StockW : kQ2StockW, quest3 ? kQ3StockH : kQ2StockH);
      QCHECK(tq::HookedRefresh() != nullptr);
      tq::HookedRefresh()();
      QCHECK(Lo(g_stored) == (quest3 ? r.q3Width : r.q2Width));
      QCHECK(Hi(g_stored) == (quest3 ? r.q3Height : r.q2Height));
      QCHECK(g_propCalls == 2);  // ids 5 and 6, through the vrapi slot, which answers the level
      QCHECK(g_origRefresh == 0);  // the reference body's cave is never run
      QCHECK(tq::CurrentCounts().served == 2);
      QCHECK(tq::LastDecision().source == tq::DecisionSource::kLevel);
    }
  }
}

void RefreshStockStoresTheRuntimesNativeSize() {
  ArmRoute(tq::Level::kStock, tq::EyeInput::kReference, &g_stored, &g_zero);
  SetStock(kQ2StockW, kQ2StockH);
  tq::HookedRefresh()();
  QCHECK(Lo(g_stored) == kQ2StockW && Hi(g_stored) == kQ2StockH);
  QCHECK(tq::CurrentCounts().served == 0);
  QCHECK(tq::LastDecision().source == tq::DecisionSource::kNative);
  QCHECK(g_origRefresh == 0);
}

void RefreshTruncatesTheScaledSizeAsTheGameDoes() {
  ArmRoute(tq::Level::kLow, tq::EyeInput::kReference, &g_stored, &g_zero);
  SetStock(kQ3StockW, kQ3StockH);
  g_scaleX = 1.0003F;  // 1680 * 1.0003 = 1680.504 and 1760 * 1.0003 = 1760.528: both truncate down
  tq::HookedRefresh()();
  QCHECK(Lo(g_stored) == 1680 && Hi(g_stored) == 1760);
}

void RefreshOverrideIsScaledAndNeverQueried() {
  const std::uint64_t overridePair = 1000ULL | (2000ULL << 32);
  ArmRoute(tq::Level::kHigh, tq::EyeInput::kReference, &g_stored, &overridePair);
  g_scaleX = 2.0F;
  tq::HookedRefresh()();
  QCHECK(Lo(g_stored) == 2000 && Hi(g_stored) == 4000);
  QCHECK(g_propCalls == 0);
  QCHECK(tq::LastDecision().source == tq::DecisionSource::kGameOverride);
  QCHECK(tq::CurrentCounts().served == 0);
  QCHECK(g_origRefresh == 0);
}

void RefreshAsksTheStartResolverOnce() {
  ArmRoute(tq::Level::kLow, tq::EyeInput::kReference, &g_stored, &g_zero);
  g_resolverCalls = 0;
  g_resolverAnswer = tq::kLevelUnset;
  tq::SetStartResolver(&Resolver);
  tq::HookedRefresh()();
  tq::HookedRefresh()();
  QCHECK(g_resolverCalls == 1);
}

void RefreshWithUnresolvedPinsCallsTheOriginalOnce() {
  ArmRoute(tq::Level::kHigh, tq::EyeInput::kReference, nullptr, &g_zero);
  tq::HookedRefresh()();
  QCHECK(g_origRefresh == 1);
  QCHECK(g_propCalls == 0);
}

void HookedRefreshIsNullUntilArmed() {
  Reset();
  QCHECK(tq::HookedRefresh() == nullptr);
  tq::ArmRefreshForTest();
  QCHECK(tq::HookedRefresh() != nullptr);
}

void TheInstallLineNamesTheInputAndTheRouteState() {
  Reset();
  tq::SetInputForTest(tq::EyeInput::kReference);
  Capture log;
  QCHECK(tq::InstallVia(tq::Level::kHigh, false, &GotAccepts));
  const std::string line = TheLine();
  QCHECK(Field(line, "input") == "reference");
  QCHECK(Field(line, "refresh") == "not_installed");  // InstallVia does not arm the RefreshRTSize slot
  QCHECK(Field(line, "status") != "<absent>");
}

}  // namespace

int main() {
  TheTableIsTheInstallersTable();
  TheHeadsetSplitsAtOneSixEightyOnBothAxes();
  OnlySevenWordsParse();
  WithNothingArmedEveryQueryIsTheRuntimesOwn();
  EachLevelLatchesAndAnswers();
  EachLevelAnswersTheTableOfTheHeadsetTheRuntimeNames();
  LowAnswersTheStockSizeOfEachHeadset();
  OnlyIds5And6AreAnsweredWithTheSizeAndTheRestPassThrough();
  TheOriginalIsCalledOnceWithTheGamesArgumentsForTheTwoIdsToo();
  TheLevelIsLatchedAndANewOneIsRefused();
  StockAnswersTheRuntimesOwnSize();
  ADisarmedHookPassesEverythingThrough();
  AThunkWithoutAnOriginalIsAFaultAndOverridesNothing();
  TheCountersCarryTheRuntimesAnswerAndWhatWeReplaced();
  ARequestChangesNothingAndACommitChangesTheNextAnswer();
  OnlyTheTwoEyeSizeIdsAreCounted();
  TheStartResolverIsAskedOnceByTheFirstQuery();
  RefreshAnswersTheLevelsSizeOnBothHeadsets();
  RefreshStockStoresTheRuntimesNativeSize();
  RefreshTruncatesTheScaledSizeAsTheGameDoes();
  RefreshOverrideIsScaledAndNeverQueried();
  RefreshAsksTheStartResolverOnce();
  RefreshWithUnresolvedPinsCallsTheOriginalOnce();
  HookedRefreshIsNullUntilArmed();
  TheInstallLineNamesTheInputAndTheRouteState();
  AnInstalledLevelLogsOneLineWithTheLevelTheSourceAndTheSize();
  StockInstallsTheHookAndAnswersTheRuntimesOwnSize();
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
