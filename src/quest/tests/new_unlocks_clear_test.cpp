// Host test for the new-unlocks clear on Quest (#2410): the real wrappers (src/quest/combat/new_unlocks_clear.cpp), built without exceptions as the
// sentinel builds it, installed by the real GotHook into the two data words of a libr15 image built in memory (the JUMP_SLOT and the GLOB_DAT
// of CR15NetGame::RemoveAllNewlyUnlockedItems), and called the way the game calls them: through the word. The game side is a model: the clear
// (count to 0 and the dirty bit, as the game's own code does), CJson::Empty, and the net game and the user laid out where the module reads
// them (the offsets are spelled in this file, not taken from the shared header, so a wrong constant there fails here).
//
// What this test does not repeat: the decisions (held, re-armed, superseded, dropped) are the shared header's
// (src/runtime/patch/new_unlocks_clear_rules.h) and its GTest (src/runtime/tests/test_new_unlocks_clear.cpp). What it pins is the Quest side of
// that rule: that the two words are the ones hooked and each wrapper calls ITS word's original with the game's argument, that the table entries are
// the store libr15's symbol and addresses, that the frame work re-arms by setting the dirty bit once the game's emptiness test says the buffer is
// empty, that a word that does not hold the clear is refused alone, that a pin that differs refuses the module before any word is touched, and
// the lines. got_pinned_test.cpp resolves the two relocations and the two code pins in the real libr15.
//
// Built and run by `just test-quest-combat`.

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/new_unlocks_clear.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"
#include "runtime/patch/new_unlocks_clear_rules.h"

namespace {

namespace nuc = nevr_quest::combat::new_unlocks_clear;
namespace frame = nevr_quest::invite::frame;
namespace pinned = sentinel::pinned;
namespace rules = nevr_new_unlocks_clear;
using combat_test::Image;
using combat_test::Reloc;

// ---- the layout and the store file's numbers, as this test spells them ----------------------------------------------------------

constexpr std::size_t kOffUser = 0x28d0;       // net game -> the user
constexpr std::size_t kOffCount = 0x51c50;     // net game: how many new unlocks the client holds
constexpr std::size_t kOffFlags = 0x9c;        // user: the flag word
constexpr std::size_t kOffInFlight = 0x28;     // user: the update sent and not yet acknowledged
constexpr std::uint32_t kDirty = 0x1;
constexpr std::uint64_t kClearVaddr = 0x1297114;
constexpr std::uint64_t kJumpSlot = 0x36ccac0;
constexpr std::uint64_t kGlobDatSlot = 0x3709e08;
constexpr std::uint64_t kEmptyVaddr = 0xfa82f8;
constexpr const char* kSymbol = "_ZN10NRadEngine8NRadGame11CR15NetGame27RemoveAllNewlyUnlockedItemsEv";

// ---- the game's memory ----------------------------------------------------------------------------------------------------------

alignas(16) unsigned char g_game[0x52000];
alignas(16) unsigned char g_user[0x100];
alignas(16) unsigned char g_otherUser[0x100];

template <typename T>
void Put(unsigned char* base, std::size_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof(value));
}
template <typename T>
T Get(const unsigned char* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

std::uint64_t Count() { return Get<std::uint64_t>(g_game, kOffCount); }
void SetCount(std::uint64_t n) { Put<std::uint64_t>(g_game, kOffCount, n); }
std::uint32_t Flags(const unsigned char* user = g_user) { return Get<std::uint32_t>(user, kOffFlags); }

bool g_inFlight = false;          // "the buffer at user + 0x28 is not empty"
int g_emptyCalls = 0;
const void* g_emptyArg = nullptr;

// ---- the game's functions (models) ---------------------------------------------------------------------------------------------

int g_clearCalls[2];
const void* g_clearArg[2];

// RemoveAllNewlyUnlockedItems: returns at once on an empty list, else the count to 0 and the user's dirty bit. One model per word, so each word's
// own stored original can be told apart.
template <int Word>
void ModelClear(void* game) {
  ++g_clearCalls[Word];
  g_clearArg[Word] = game;
  auto* g = static_cast<unsigned char*>(game);
  if (Get<std::uint64_t>(g, kOffCount) == 0) return;
  Put<std::uint64_t>(g, kOffCount, 0);
  const auto user = Get<std::uintptr_t>(g, kOffUser);
  if (user != 0) Put<std::uint32_t>(reinterpret_cast<unsigned char*>(user), kOffFlags, Get<std::uint32_t>(reinterpret_cast<unsigned char*>(user), kOffFlags) | kDirty);
}

// CJson::Empty: nonzero when the json has nothing in it. The module asks it about user + 0x28.
int ModelJsonEmpty(void* json) {
  ++g_emptyCalls;
  g_emptyArg = json;
  return g_inFlight ? 0 : 1;
}

void ModelForeign() {}  // a word some other runtime put in a slot: an executable address

// ---- the libr15 image, in memory -----------------------------------------------------------------------------------------------

Image g_image;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
bool NoLibr15(const char*, sentinel::ElfImage*) { return false; }

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

std::uintptr_t Base() { return g_image.Base(); }
std::uintptr_t Word(int word) { return g_image.Get<std::uintptr_t>(word == 0 ? kJumpSlot : kGlobDatSlot); }
using ClearFn = void (*)(void*);
// The game calls the clear through the word.
void CallWord(int word) { reinterpret_cast<ClearFn>(Word(word))(g_game); }

struct ImageOptions {
  const char* buildId = nullptr;  // nullptr: the pinned patched build ID
  int flippedPin = -1;            // index into kNewUnlocksClearCode whose last word differs
  const void* foreignWord = nullptr;  // put into the GLOB_DAT word instead of the clear
};

void BuildImage(const ImageOptions& opt) {
  g_image.Build({{kSymbol, kJumpSlot, false, false, kClearVaddr}, {kSymbol, kGlobDatSlot, true, false, kClearVaddr}}, opt.buildId);
  for (std::size_t i = 0; i < pinned::kNewUnlocksClearCodeCount; ++i) {
    const pinned::PinnedCode& pin = pinned::kNewUnlocksClearCode[i];
    for (std::size_t w = 0; w < pin.words; ++w) {
      std::uint32_t word = pin.code[w];
      if (static_cast<int>(i) == opt.flippedPin && w + 1 == pin.words) word ^= 0x00000020U;
      g_image.Put<std::uint32_t>(pin.vaddr + w * 4, word);
    }
  }
  g_image.Put<std::uintptr_t>(kJumpSlot, Base() + kClearVaddr);
  g_image.Put<std::uintptr_t>(kGlobDatSlot, opt.foreignWord != nullptr ? reinterpret_cast<std::uintptr_t>(opt.foreignWord) : Base() + kClearVaddr);
  g_image.Put<std::uintptr_t>(kJumpSlot - 8, 0x1111);  // neighbours, never touched
  g_image.Put<std::uintptr_t>(kGlobDatSlot + 8, 0x2222);
}

void BuildGame() {
  std::memset(g_game, 0, sizeof(g_game));
  std::memset(g_user, 0, sizeof(g_user));
  std::memset(g_otherUser, 0, sizeof(g_otherUser));
  Put<std::uintptr_t>(g_game, kOffUser, reinterpret_cast<std::uintptr_t>(g_user));
  g_inFlight = false;
  g_emptyCalls = 0;
  g_emptyArg = nullptr;
  g_clearCalls[0] = g_clearCalls[1] = 0;
  g_clearArg[0] = g_clearArg[1] = nullptr;
}

void Begin(const ImageOptions& opt = {}) {
  g_lines.clear();
  nuc::ResetForTest();
  frame::ResetForTest();
  const nuc::GameFunctions fns{reinterpret_cast<std::uintptr_t>(&ModelJsonEmpty)};
  nuc::SetGameFunctionsForTest(&fns);
  sentinel::SetLogSink(&Capture);
  BuildGame();
  BuildImage(opt);
}
void End() {
  nuc::ResetForTest();
  frame::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

// The install and the models standing in for the game's original: each word's thunk keeps its own original, and the test points both at models.
bool InstallWithModels() {
  const bool ok = nuc::InstallAt(&Lookup);
  *pinned::LibR15NewUnlocksClearThunk<0>::OriginalOut() = reinterpret_cast<void*>(&ModelClear<0>);
  *pinned::LibR15NewUnlocksClearThunk<1>::OriginalOut() = reinterpret_cast<void*>(&ModelClear<1>);
  return ok;
}

// The game's SaveClientProfileChanges, for a change made while an update is in flight: it merges the change into the in-flight buffer and clears the bit
// without a send (the reason for the module). The test applies it by hand between two frames.
void GameMergesAndClearsTheBit() { Put<std::uint32_t>(g_user, kOffFlags, Flags() & ~kDirty); }

// ---- the tests ------------------------------------------------------------------------------------------------------------------

// The numbers and names this module was written against: the store libr15's symbol, addresses and code words, and the same addresses in the rules header.
void TheTableEntriesAreTheStoreLibr15s() {
  QCHECK(std::strcmp(pinned::kNewUnlocksClear.symbol, kSymbol) == 0);
  QCHECK(pinned::kNewUnlocksClear.vaddr == kClearVaddr && pinned::kNewUnlocksClear.jumpSlot == kJumpSlot && pinned::kNewUnlocksClear.globDatSlot == kGlobDatSlot);
  QCHECK(pinned::kJsonEmptyVaddr == kEmptyVaddr);
  QCHECK(rules::kQuestAddresses.clear == kClearVaddr && rules::kQuestAddresses.jsonEmpty == kEmptyVaddr);
  const sentinel::GotTarget jump = pinned::LibR15NewUnlocksClear(sentinel::RelocKind::kJumpSlot);
  const sentinel::GotTarget glob = pinned::LibR15NewUnlocksClear(sentinel::RelocKind::kGlobDat);
  QCHECK(std::strcmp(jump.symbol, kSymbol) == 0 && std::strcmp(glob.symbol, kSymbol) == 0);
  QCHECK(jump.slotVaddr.has_value() && *jump.slotVaddr == kJumpSlot && glob.slotVaddr.has_value() && *glob.slotVaddr == kGlobDatSlot);
  QCHECK(pinned::kNewUnlocksClearCodeCount == 2U);
  QCHECK(pinned::kNewUnlocksClearCode[0].vaddr == kClearVaddr && pinned::kNewUnlocksClearCode[1].vaddr == kEmptyVaddr);
  // The clear's first words build the count's offset: mov w8,#0x1c50; movk w8,#5,lsl #16 (= 0x51c50).
  QCHECK(pinned::kNewUnlocksClearCode[0].code[1] == 0x52838a08U && pinned::kNewUnlocksClearCode[0].code[2] == 0x72a000a8U);
  QCHECK(kOffCount == 0x51c50U && rules::kNewUnlocksCountOffset == kOffCount);
  QCHECK(rules::kNetGameUserOffset == kOffUser && rules::kUserFlagsOffset == kOffFlags && rules::kUserInFlightOffset == kOffInFlight &&
         rules::kUserDirtyBit == kDirty);
}

// Both words hold a thunk, each keeps the clear as its original, the neighbours are untouched, and the install line says so.
void AnInstallPutsAThunkInBothWordsAndKeepsTheOriginal() {
  Begin();
  QCHECK(InstallWithModels());
  QCHECK(Word(0) == reinterpret_cast<std::uintptr_t>(pinned::LibR15NewUnlocksClearThunk<0>::EntryAddress()));
  QCHECK(Word(1) == reinterpret_cast<std::uintptr_t>(pinned::LibR15NewUnlocksClearThunk<1>::EntryAddress()));
  QCHECK(g_image.Get<std::uintptr_t>(kJumpSlot - 8) == 0x1111 && g_image.Get<std::uintptr_t>(kGlobDatSlot + 8) == 0x2222);
  QCHECK(nuc::PublishedGameFunctions().jsonEmpty == reinterpret_cast<std::uintptr_t>(&ModelJsonEmpty));
  const std::vector<Line> install = Lines("combat_new_unlocks_clear_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  QCHECK(Golden(install[0].text) ==
         "\"event\":\"combat_new_unlocks_clear_install\",\"result\":\"installed\",\"jump_slot\":\"ok\",\"glob_dat\":\"ok\","
         "\"consequence\":\"a clear made while a profile update is unacknowledged is sent after the acknowledgement\"}");
  End();
}

// With nothing in flight the game sends the clear itself: the original runs with the game's argument, no line is written, nothing is held.
void AClearWithNothingInFlightIsLeftToTheGame() {
  for (int word = 0; word < 2; ++word) {
    Begin();
    QCHECK(InstallWithModels());
    SetCount(3);
    CallWord(word);
    QCHECK(g_clearCalls[word] == 1 && g_clearCalls[1 - word] == 0);  // each word calls ITS original
    QCHECK(g_clearArg[word] == g_game);
    QCHECK(Count() == 0 && Flags() == kDirty);                       // the list emptied and the game's bit is set
    QCHECK(g_emptyCalls == 1 && g_emptyArg == g_user + kOffInFlight);  // asked once, about user + 0x28
    frame::Tick();
    QCHECK(g_emptyCalls == 1);                                       // nothing held: the frame work asks the game nothing
    QCHECK(Lines("combat_new_unlocks_clear_held").empty() && Lines("combat_new_unlocks_clear_rearmed").empty());  // no line of ours
    End();
  }
}

// A clear of an empty list changed nothing: even with an update in flight it is not held.
void AClearOfAnEmptyListIsNotHeld() {
  Begin();
  QCHECK(InstallWithModels());
  g_inFlight = true;
  SetCount(0);
  CallWord(0);
  frame::Tick();
  QCHECK(Lines("combat_new_unlocks_clear_held").empty() && Flags() == 0);
  End();
}

// The reason for the module, on each word: a clear while an update is in flight is held; the game merges it and clears the bit; once the buffer is empty
// again (the acknowledgement) the frame work sets the bit, once, and the held clear ends in one line.
void AHeldClearIsRearmedOnceAfterTheAcknowledgement() {
  for (int word = 0; word < 2; ++word) {
    Begin();
    QCHECK(InstallWithModels());
    g_inFlight = true;
    SetCount(5);
    CallWord(word);
    QCHECK(g_clearCalls[word] == 1 && Count() == 0);                 // the clear ran at once
    const std::vector<Line> held = Lines("combat_new_unlocks_clear_held");
    QCHECK(held.size() == 1U && held[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(held[0].text) ==
           std::string("\"event\":\"combat_new_unlocks_clear_held\",\"msg\":\"") + rules::kMsgHeld + "\",\"user_id\":0,\"count\":5}");
    GameMergesAndClearsTheBit();
    frame::Tick();                                                   // still unacknowledged: nothing changes
    QCHECK(Flags() == 0 && Lines("combat_new_unlocks_clear_rearmed").empty());
    g_inFlight = false;                                              // the acknowledgement reset the buffer
    frame::Tick();
    QCHECK(Flags() == kDirty);                                       // the bit is set again
    const std::vector<Line> rearmed = Lines("combat_new_unlocks_clear_rearmed");
    QCHECK(rearmed.size() == 1U && rearmed[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(rearmed[0].text) ==
           std::string("\"event\":\"combat_new_unlocks_clear_rearmed\",\"msg\":\"") + rules::kMsgRearmed + "\",\"user_id\":0,\"count\":5}");
    Put<std::uint32_t>(g_user, kOffFlags, 0);                        // the game sent it and cleared the bit
    frame::Tick();
    QCHECK(Flags() == 0 && Lines("combat_new_unlocks_clear_rearmed").size() == 1U);  // once
    End();
  }
}

// An add after the clear puts an entry back: the held clear is dropped, the bit is not set, one line says why.
void AnAddAfterTheClearSupersedesIt() {
  Begin();
  QCHECK(InstallWithModels());
  g_inFlight = true;
  SetCount(4);
  CallWord(0);
  GameMergesAndClearsTheBit();
  SetCount(1);  // the add
  frame::Tick();
  const std::vector<Line> out = Lines("combat_new_unlocks_clear_superseded");
  QCHECK(out.size() == 1U && out[0].level == sentinel::LogLevel::kInfo);
  QCHECK(Golden(out[0].text) ==
         std::string("\"event\":\"combat_new_unlocks_clear_superseded\",\"msg\":\"") + rules::kMsgSuperseded + "\",\"user_id\":0,\"count\":4}");
  g_inFlight = false;
  frame::Tick();
  QCHECK(Flags() == 0 && Lines("combat_new_unlocks_clear_rearmed").empty());
  End();
}

// The user the clear was held for is gone (the net game's user is another object, or null): the clear is dropped, nothing of either user is written.
void AGoneUserDropsTheHoldAndNothingIsWritten() {
  for (int gone = 0; gone < 2; ++gone) {
    Begin();
    QCHECK(InstallWithModels());
    g_inFlight = true;
    SetCount(2);
    CallWord(1);
    GameMergesAndClearsTheBit();
    Put<std::uintptr_t>(g_game, kOffUser, gone == 0 ? reinterpret_cast<std::uintptr_t>(g_otherUser) : 0);
    g_inFlight = false;
    frame::Tick();
    const std::vector<Line> out = Lines("combat_new_unlocks_clear_dropped");
    QCHECK(out.size() == 1U && out[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(out[0].text) ==
           std::string("\"event\":\"combat_new_unlocks_clear_dropped\",\"msg\":\"") + rules::kMsgDropped + "\",\"user_id\":0,\"count\":2}");
    QCHECK(Flags() == 0 && Flags(g_otherUser) == 0 && Lines("combat_new_unlocks_clear_rearmed").empty());
    End();
  }
}

// The user's page is read-only when the poll runs (the object was unmapped or remapped since the clear): the module must not write through it. The
// clear is dropped, the word is left as it is and nothing faults; a write through the page would end the test with a signal.
void AUserWhosePageCannotBeWrittenIsDropped() {
  Begin();
  QCHECK(InstallWithModels());
  void* const page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  QCHECK(page != MAP_FAILED);
  auto* user = static_cast<unsigned char*>(page);
  Put<std::uintptr_t>(g_game, kOffUser, reinterpret_cast<std::uintptr_t>(user));
  g_inFlight = true;
  SetCount(2);
  CallWord(0);                                  // held: the page is readable and writable now
  QCHECK(Lines("combat_new_unlocks_clear_held").size() == 1U);
  Put<std::uint32_t>(user, kOffFlags, 0);       // the game merged the change and cleared the bit
  QCHECK(mprotect(page, 4096, PROT_READ) == 0);  // the page can no longer be written
  g_inFlight = false;
  frame::Tick();
  QCHECK(Lines("combat_new_unlocks_clear_dropped").size() == 1U && Lines("combat_new_unlocks_clear_rearmed").empty());
  QCHECK(Flags(user) == 0);
  QCHECK(mprotect(page, 4096, PROT_READ | PROT_WRITE) == 0);
  End();
  QCHECK(munmap(page, 4096) == 0);
}

// The refusals: a pin that differs refuses the module before any word is touched; so do a missing library and another build; a word that does not hold
// the clear is refused alone and the other still re-arms.
void TheRefusalsSayWhy() {
  for (std::size_t pin = 0; pin < pinned::kNewUnlocksClearCodeCount; ++pin) {
    ImageOptions opt;
    opt.flippedPin = static_cast<int>(pin);
    Begin(opt);
    QCHECK(!nuc::InstallAt(&Lookup));
    QCHECK(Word(0) == Base() + kClearVaddr && Word(1) == Base() + kClearVaddr);  // no word touched
    const std::vector<Line> out = Lines("combat_new_unlocks_clear_install");
    QCHECK(out.size() == 1U && out[0].level == sentinel::LogLevel::kError);
    QCHECK(Has(out[0].text, "\"why\":\"pin_mismatch\"") && Has(out[0].text, pinned::kNewUnlocksClearCode[pin].name));
    End();
  }
  Begin();
  QCHECK(!nuc::InstallAt(&NoLibr15));
  QCHECK(Has(Lines("combat_new_unlocks_clear_install")[0].text, "\"why\":\"module_not_loaded\""));
  End();
  ImageOptions other;
  other.buildId = "0000000000000000000000000000000000000000";
  Begin(other);
  QCHECK(!nuc::InstallAt(&Lookup));
  QCHECK(Has(Lines("combat_new_unlocks_clear_install")[0].text, "\"why\":\"build_id_mismatch\""));
  End();
  ImageOptions foreign;
  foreign.foreignWord = reinterpret_cast<const void*>(&ModelForeign);
  Begin(foreign);
  QCHECK(!InstallWithModels());
  QCHECK(Word(0) == reinterpret_cast<std::uintptr_t>(pinned::LibR15NewUnlocksClearThunk<0>::EntryAddress()));  // the JUMP_SLOT went in
  QCHECK(Word(1) == reinterpret_cast<std::uintptr_t>(&ModelForeign));                                           // the foreign word was left alone
  const std::vector<Line> out = Lines("combat_new_unlocks_clear_install");
  QCHECK(out.size() == 1U && out[0].level == sentinel::LogLevel::kError);
  QCHECK(Has(out[0].text, "\"result\":\"partial\"") && Has(out[0].text, "\"jump_slot\":\"ok\""));
  g_inFlight = true;
  SetCount(2);
  CallWord(0);
  GameMergesAndClearsTheBit();
  g_inFlight = false;
  frame::Tick();
  QCHECK(Flags() == kDirty);  // the word that went in still re-arms
  End();
  QCHECK(std::strcmp(nuc::InstallResultName(2), "installed") == 0 && std::strcmp(nuc::InstallResultName(1), "partial") == 0 &&
         std::strcmp(nuc::InstallResultName(0), "refused") == 0);
}

}  // namespace

int main() {
  TheTableEntriesAreTheStoreLibr15s();
  AnInstallPutsAThunkInBothWordsAndKeepsTheOriginal();
  AClearWithNothingInFlightIsLeftToTheGame();
  AClearOfAnEmptyListIsNotHeld();
  AHeldClearIsRearmedOnceAfterTheAcknowledgement();
  AnAddAfterTheClearSupersedesIt();
  AGoneUserDropsTheHoldAndNothingIsWritten();
  AUserWhosePageCannotBeWrittenIsDropped();
  TheRefusalsSayWhy();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "new_unlocks_clear_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("new_unlocks_clear_test: ok\n");
  return 0;
}
