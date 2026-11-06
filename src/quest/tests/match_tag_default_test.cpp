// Host test for "combat name tags are on by default" (#2090): the real handlers (src/quest/combat/match_tags.cpp), built without
// exceptions as the sentinel builds it, driven through the two thunks' entries with a stand-in for libr15's CR15Game constructor
// that lays the default settings down where the game's does (flags at this+0x8c08 and this+0x8ce4). No GOT slot is touched:
// got_pinned_test.cpp resolves both pinned slots in the real library.
//
// What it pins: bit 13 is set in a copy only when the copy holds exactly the stock default 0x1003, each copy on its own; the
// original runs first, so the words the handler reads are the ones the constructor wrote; no byte of the object but those two
// words changes; a settings load after the constructor (a saved false) is not undone; a null object and an unarmed thunk are left
// alone; the line is written once per construction with the golden fields; a slot that cannot be found refuses with its status in
// the install line.
//
// Built and run by `just test-quest-combat`.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/combat/match_tags.h"
#include "quest/tests/test_check.h"

namespace {

namespace mt = nevr_quest::combat::match_tags;

mt::C1Thunk::Fn C1Entry() { return reinterpret_cast<mt::C1Thunk::Fn>(mt::C1Thunk::EntryAddress()); }
mt::C2Thunk::Fn C2Entry() { return reinterpret_cast<mt::C2Thunk::Fn>(mt::C2Thunk::EntryAddress()); }

// The offsets the constructor stores the two literals at (store libr15, 0x11fce34): the flags words, as the test spells them
// (not as the header does, so a wrong constant in the header fails here).
constexpr std::size_t kCurrent = 0x8c08;
constexpr std::size_t kPending = 0x8ce4;
constexpr std::size_t kObjectBytes = 0x9100;  // the object is 0x90c8 bytes; a little more room, the canaries below sit inside it
constexpr unsigned char kFill = 0x5A;

// ---- the game's side ------------------------------------------------------------------------------------------------------

// What the stand-in constructor writes into the two words (the stock default unless a test says otherwise).
std::uint32_t g_ctorCurrent = 0x1003;
std::uint32_t g_ctorPending = 0x1003;
int g_ctorCalls = 0;
void* g_ctorGame = nullptr;
const void* g_ctorArgs = nullptr;

void Put32(unsigned char* object, std::size_t offset, std::uint32_t value) { std::memcpy(object + offset, &value, sizeof(value)); }
std::uint32_t Get32(const unsigned char* object, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, object + offset, sizeof(value));
  return value;
}

// CR15Game::CR15Game: everything the constructor leaves is a fill byte; the two flags words are what it stores. The words next to
// them are the rest of the two 16-byte literals ({0, 0x1003, 1, 0} at +0x8c04, {0x1003, 1, 0, 0xa} at +0x8ce4).
void StandInCtor(void* game, const void* args) {
  ++g_ctorCalls;
  g_ctorGame = game;
  g_ctorArgs = args;
  if (game == nullptr) return;
  unsigned char* object = static_cast<unsigned char*>(game);
  std::memset(object, kFill, kObjectBytes);
  Put32(object, kCurrent - 4, 0);
  Put32(object, kCurrent, g_ctorCurrent);
  Put32(object, kCurrent + 4, 1);
  Put32(object, kCurrent + 8, 0);
  Put32(object, kPending, g_ctorPending);
  Put32(object, kPending + 4, 1);
  Put32(object, kPending + 8, 0);
  Put32(object, kPending + 12, 0xa);
}

alignas(16) unsigned char g_object[kObjectBytes];
alignas(16) unsigned char g_expected[kObjectBytes];
int g_mainArgs = 0;  // an address to pass as the CMainArgs reference

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel /*level*/, const char* line) { g_lines.emplace_back(line); }

void Reset() {
  g_ctorCurrent = 0x1003;
  g_ctorPending = 0x1003;
  g_ctorCalls = 0;
  g_ctorGame = nullptr;
  g_ctorArgs = nullptr;
  g_lines.clear();
  std::memset(g_object, 0, sizeof(g_object));
  mt::ResetForTest();
  *mt::C1Thunk::OriginalOut() = reinterpret_cast<void*>(&StandInCtor);
  *mt::C2Thunk::OriginalOut() = reinterpret_cast<void*>(&StandInCtor);
}

// The object exactly as the stand-in constructor leaves it, with the two flags words set to what the test expects after the hook.
void Expect(std::uint32_t current, std::uint32_t pending) {
  const std::uint32_t savedCurrent = g_ctorCurrent;
  const std::uint32_t savedPending = g_ctorPending;
  StandInCtor(g_expected, nullptr);
  --g_ctorCalls;
  g_ctorCurrent = savedCurrent;
  g_ctorPending = savedPending;
  Put32(g_expected, kCurrent, current);
  Put32(g_expected, kPending, pending);
}

// A line from the event on, without the time stamp and level in front of it.
std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

const std::string* LineOf(const char* event) {
  for (const std::string& line : g_lines) {
    if (line.find(std::string("\"event\":\"") + event + "\"") != std::string::npos) return &line;
  }
  return nullptr;
}

int CountOf(const char* event) {
  int n = 0;
  for (const std::string& line : g_lines) n += line.find(std::string("\"event\":\"") + event + "\"") != std::string::npos ? 1 : 0;
  return n;
}

// ---- tests --------------------------------------------------------------------------------------------------------------

// The rule on its own: the stock default gets bit 13, every other word is returned as it is.
void TheRuleSetsBit13OnlyOnTheStockDefault() {
  QCHECK(mt::DefaultedFlags(0x1003) == 0x3003);
  QCHECK(mt::DefaultedFlags(0x3003) == 0x3003);  // already set
  QCHECK(mt::DefaultedFlags(0x1003 | 0x4000) == (0x1003 | 0x4000));  // the load's own bit: not the stock word
  QCHECK(mt::DefaultedFlags(0x1002) == 0x1002);
  QCHECK(mt::DefaultedFlags(0x1004) == 0x1004);
  QCHECK(mt::DefaultedFlags(0x0003) == 0x0003);  // a player-style value with the tags off
  QCHECK(mt::DefaultedFlags(0) == 0);
  QCHECK(mt::DefaultedFlags(0xffffffffU) == 0xffffffffU);
}

// The header's constants are the ones the disassembly gives.
void TheConstantsAreTheStoreLibr15s() {
  QCHECK(mt::kCurrentFlagsOffset == kCurrent);
  QCHECK(mt::kPendingFlagsOffset == kPending);
  QCHECK(mt::kStockFlags == 0x1003);
  QCHECK(mt::kMatchTagDisplayBit == 0x2000);
}

// A new CR15Game starts with bit 13 set in both copies, through the C1 entry (the game's own construction), and nothing else of the
// object differs from what the constructor wrote.
void ABareConstructionStartsWithTheTagsOnInBothCopies() {
  Reset();
  mt::ArmForTest();
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(g_ctorCalls == 1);
  QCHECK(g_ctorGame == g_object);
  QCHECK(g_ctorArgs == &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);
  QCHECK(Get32(g_object, kPending) == 0x3003);
  Expect(0x3003, 0x3003);
  QCHECK(std::memcmp(g_object, g_expected, kObjectBytes) == 0);
}

// The same through C2, the base-object constructor the server classes call.
void TheBaseObjectConstructorGetsTheSameDefault() {
  Reset();
  mt::ArmForTest();
  C2Entry()(g_object, &g_mainArgs);
  QCHECK(g_ctorCalls == 1);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);
  QCHECK(Get32(g_object, kPending) == 0x3003);
  Expect(0x3003, 0x3003);
  QCHECK(std::memcmp(g_object, g_expected, kObjectBytes) == 0);
  const std::string* line = LineOf("combat_match_tags_default");
  QCHECK(line != nullptr && line->find("\"ctor\":\"c2\"") != std::string::npos);
}

// The original runs before the handler reads a word: the words it reads are the constructor's, not the object's earlier bytes.
// Were the handler first, the zeroed object would hold 0 and 0, stay 0, and the constructor would then store the stock default.
void TheOriginalRunsFirst() {
  Reset();
  mt::ArmForTest();
  Put32(g_object, kCurrent, 0xdeadbeefU);
  Put32(g_object, kPending, 0xdeadbeefU);
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);
  QCHECK(Get32(g_object, kPending) == 0x3003);
}

// Each copy on its own: a copy that does not hold the stock word is left, the other is set.
void EachCopyIsDecidedOnItsOwn() {
  Reset();
  mt::ArmForTest();
  g_ctorCurrent = 0x1003;
  g_ctorPending = 0x1103;
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);
  QCHECK(Get32(g_object, kPending) == 0x1103);
  Expect(0x3003, 0x1103);
  QCHECK(std::memcmp(g_object, g_expected, kObjectBytes) == 0);

  Reset();
  mt::ArmForTest();
  g_ctorCurrent = 0x0003;
  g_ctorPending = 0x1003;
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x0003);
  QCHECK(Get32(g_object, kPending) == 0x3003);
}

// A word that is not the stock default is never touched, whatever it holds. A mutant that sets the bit unconditionally fails here.
void AWordThatIsNotTheStockDefaultIsLeftAlone() {
  const std::uint32_t words[] = {0x0000, 0x0003, 0x1002, 0x1004, 0x2003, 0x3003, 0x5003, 0x100003, 0xffffffffU};
  for (const std::uint32_t word : words) {
    Reset();
    mt::ArmForTest();
    g_ctorCurrent = word;
    g_ctorPending = word;
    C1Entry()(g_object, &g_mainArgs);
    QCHECK(Get32(g_object, kCurrent) == word);
    QCHECK(Get32(g_object, kPending) == word);
    Expect(word, word);
    QCHECK(std::memcmp(g_object, g_expected, kObjectBytes) == 0);
  }
}

// A documentation test, not a proof: the load below is this test's own stand-in for CR15Game::LoadSettings (FromJson on the current
// copy, then current over pending), so it cannot fail if the game's real order changed. It shows only that the hook keeps nothing
// that could put the bit back after a later clear. That the real LoadSettings runs after the constructor is read from the
// disassembly (match_tags.h) and is the headset row's claim (the saved-false run).
void ASavedFalseIsNotUndone() {
  Reset();
  mt::ArmForTest();
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);
  // LoadSettings: the file says MatchTagDisplay false, so InspectFlagBit clears the bit; then the pending copy takes the current one.
  Put32(g_object, kCurrent, Get32(g_object, kCurrent) & ~0x2000U);
  Put32(g_object, kPending, Get32(g_object, kCurrent));
  QCHECK(Get32(g_object, kCurrent) == 0x1003);
  QCHECK(Get32(g_object, kPending) == 0x1003);
  // Nothing in the hook runs again for this object: a later call through the entry on another object does not reach back.
  QCHECK(CountOf("combat_match_tags_default") == 1);
}

// A null object is passed straight to the original and nothing is read or written; no line.
void ANullObjectIsLeftAlone() {
  Reset();
  mt::ArmForTest();
  C1Entry()(nullptr, &g_mainArgs);
  QCHECK(g_ctorCalls == 1);
  QCHECK(g_ctorGame == nullptr);
  QCHECK(CountOf("combat_match_tags_default") == 0);
}

// Without the handler armed the entries are the game's constructor and nothing else: the stock default stays the stock default.
void UnarmedTheEntriesArePassThrough() {
  Reset();
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(g_ctorCalls == 1);
  QCHECK(Get32(g_object, kCurrent) == 0x1003);
  QCHECK(Get32(g_object, kPending) == 0x1003);
  C2Entry()(g_object, &g_mainArgs);
  QCHECK(g_ctorCalls == 2);
  QCHECK(Get32(g_object, kCurrent) == 0x1003);
  QCHECK(Get32(g_object, kPending) == 0x1003);
  QCHECK(CountOf("combat_match_tags_default") == 0);
}

// The line says what each copy held before and after, whether or not anything changed.
void TheLineIsGoldenWhenTheTagsAreSet() {
  Reset();
  mt::ArmForTest();
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(CountOf("combat_match_tags_default") == 1);
  const std::string* line = LineOf("combat_match_tags_default");
  QCHECK(line != nullptr);
  if (line == nullptr) return;
  QCHECK(Golden(*line) ==
         "\"event\":\"combat_match_tags_default\",\"ctor\":\"c1\",\"result\":\"set\",\"changed\":2,"
         "\"current_before\":\"0x00001003\",\"current_after\":\"0x00003003\","
         "\"pending_before\":\"0x00001003\",\"pending_after\":\"0x00003003\"}");
  QCHECK(line->find("\"level\":\"info\"") != std::string::npos);
}

void TheLineIsGoldenWhenNothingChanges() {
  Reset();
  mt::ArmForTest();
  g_ctorCurrent = 0x0003;
  g_ctorPending = 0x0003;
  C2Entry()(g_object, &g_mainArgs);
  const std::string* line = LineOf("combat_match_tags_default");
  QCHECK(line != nullptr);
  if (line == nullptr) return;
  QCHECK(Golden(*line) ==
         "\"event\":\"combat_match_tags_default\",\"ctor\":\"c2\",\"result\":\"kept\",\"changed\":0,"
         "\"current_before\":\"0x00000003\",\"current_after\":\"0x00000003\","
         "\"pending_before\":\"0x00000003\",\"pending_after\":\"0x00000003\"}");
}

void ThePartialLineCountsOneCopy() {
  Reset();
  mt::ArmForTest();
  g_ctorCurrent = 0x1003;
  g_ctorPending = 0x1103;
  C1Entry()(g_object, &g_mainArgs);
  const std::string* line = LineOf("combat_match_tags_default");
  QCHECK(line != nullptr);
  if (line == nullptr) return;
  QCHECK(Golden(*line) ==
         "\"event\":\"combat_match_tags_default\",\"ctor\":\"c1\",\"result\":\"set\",\"changed\":1,"
         "\"current_before\":\"0x00001003\",\"current_after\":\"0x00003003\","
         "\"pending_before\":\"0x00001103\",\"pending_after\":\"0x00001103\"}");
}

// One line per construction: a second construction writes a second line, a null object none.
void OneLinePerConstruction() {
  Reset();
  mt::ArmForTest();
  C1Entry()(g_object, &g_mainArgs);
  C2Entry()(g_object, &g_mainArgs);
  QCHECK(CountOf("combat_match_tags_default") == 2);
}

// A slot that cannot be found refuses, leaves the game's constructor call as it is, and says why in the install line.
void AnInstallThatFindsNoSlotRefusesAndSaysWhy() {
  Reset();
  const sentinel::GotTarget c1{"libmatch_tags_not_loaded.so", sentinel::pinned::kCR15GameC1Symbol, sentinel::RelocKind::kJumpSlot};
  const sentinel::GotTarget c2{"libmatch_tags_not_loaded.so", sentinel::pinned::kCR15GameC2Symbol, sentinel::RelocKind::kJumpSlot};
  QCHECK(!mt::InstallAt(c1, c2));
  const std::string* line = LineOf("combat_match_tags_install");
  QCHECK(line != nullptr);
  if (line == nullptr) return;
  QCHECK(Golden(*line) ==
         "\"event\":\"combat_match_tags_install\",\"result\":\"refused\",\"c1\":\"module_not_loaded\",\"c2\":\"module_not_loaded\"}");
  // A refused install leaves the handlers off: a construction after it is the game's own.
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x1003);
}

// One slot refused, the other installed: the refused slot's handler is off and the installed one's stays on, whichever it is. Were a
// refused slot's handler left armed, a construction through it would be changed although its install was refused.
void ARefusedSlotsHandlerStaysOffAndTheOthersOn() {
  Reset();
  mt::ArmForTest();
  QCHECK(!mt::SettleInstall(sentinel::GotStatus::kOk, sentinel::GotStatus::kSlotChanged));
  C2Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x1003);  // C2 refused: the game's own constructor
  QCHECK(Get32(g_object, kPending) == 0x1003);
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);  // C1 installed: defaulted
  QCHECK(Get32(g_object, kPending) == 0x3003);
  const std::string* line = LineOf("combat_match_tags_install");
  QCHECK(line != nullptr);
  if (line != nullptr) {
    QCHECK(Golden(*line) == "\"event\":\"combat_match_tags_install\",\"result\":\"partial\",\"c1\":\"ok\",\"c2\":\"slot_changed\"}");
  }

  Reset();
  mt::ArmForTest();
  QCHECK(!mt::SettleInstall(sentinel::GotStatus::kSymbolNotFound, sentinel::GotStatus::kOk));
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x1003);  // C1 refused
  C2Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);  // C2 installed

  Reset();
  mt::ArmForTest();
  QCHECK(mt::SettleInstall(sentinel::GotStatus::kOk, sentinel::GotStatus::kOk));
  C1Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);
  C2Entry()(g_object, &g_mainArgs);
  QCHECK(Get32(g_object, kCurrent) == 0x3003);
}

// The install line's result: both slots, one slot (either), neither.
void TheInstallResultNamesHowManySlotsTookTheirThunk() {
  QCHECK(std::strcmp(mt::InstallResultName(true, true), "installed") == 0);
  QCHECK(std::strcmp(mt::InstallResultName(true, false), "partial") == 0);
  QCHECK(std::strcmp(mt::InstallResultName(false, true), "partial") == 0);
  QCHECK(std::strcmp(mt::InstallResultName(false, false), "refused") == 0);
}

// The pinned targets name the two constructor symbols and the two link-time slots of the store libr15.
void ThePinnedTargetsAreTheStoreLibr15s() {
  const sentinel::GotTarget c1 = sentinel::pinned::LibR15CR15GameC1();
  const sentinel::GotTarget c2 = sentinel::pinned::LibR15CR15GameC2();
  QCHECK(std::strcmp(c1.symbol, "_ZN10NRadEngine8NRadGame8CR15GameC1ERKNS_9CMainArgsE") == 0);
  QCHECK(std::strcmp(c2.symbol, "_ZN10NRadEngine8NRadGame8CR15GameC2ERKNS_9CMainArgsE") == 0);
  QCHECK(c1.slotVaddr.has_value() && *c1.slotVaddr == 0x36c0018ULL);
  QCHECK(c2.slotVaddr.has_value() && *c2.slotVaddr == 0x36e7828ULL);
  QCHECK(c1.kind == sentinel::RelocKind::kJumpSlot && c2.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(std::strcmp(c1.module, "libr15.so") == 0 && std::strcmp(c2.module, "libr15.so") == 0);
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureLine);
  TheRuleSetsBit13OnlyOnTheStockDefault();
  TheConstantsAreTheStoreLibr15s();
  ABareConstructionStartsWithTheTagsOnInBothCopies();
  TheBaseObjectConstructorGetsTheSameDefault();
  TheOriginalRunsFirst();
  EachCopyIsDecidedOnItsOwn();
  AWordThatIsNotTheStockDefaultIsLeftAlone();
  ASavedFalseIsNotUndone();
  ANullObjectIsLeftAlone();
  UnarmedTheEntriesArePassThrough();
  TheLineIsGoldenWhenTheTagsAreSet();
  TheLineIsGoldenWhenNothingChanges();
  ThePartialLineCountsOneCopy();
  OneLinePerConstruction();
  AnInstallThatFindsNoSlotRefusesAndSaysWhy();
  ARefusedSlotsHandlerStaysOffAndTheOthersOn();
  TheInstallResultNamesHowManySlotsTookTheirThunk();
  ThePinnedTargetsAreTheStoreLibr15s();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "match_tag_default_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("match_tag_default_test: ok\n");
  return 0;
}
