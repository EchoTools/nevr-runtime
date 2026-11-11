// Host test of the global_mp level-name module (src/quest/combat/levelname.cpp, #2181): the real handlers through four thunks, installed by the real
// GotHook into the four vtable words of a libr15 image built in memory (R_AARCH64_ABS64 relocations at the pinned addresses), and called the way
// the game calls them: through the word. The game side is a model: the function the words hold (it appends "_lowspec", modelled as a xor) and the
// plain function (it returns the name), the CR15Game object with its lowspec flags word. The addresses are spelled in this file, not taken from
// the header, so a wrong constant there fails here.
//
// What it pins: each of the four words (and only those) holds a thunk and calls its own original; the rule (global_mp with bit 31 of game+0x7ad8 set
// gets the plain function, anything else the original, with the game's arguments); an unreadable game object never faults; the counter and the first
// sixteen lines; all four words or none (a word that holds anything else refuses the module and puts the others back); the other refusals; the switch;
// the plugin's kill file read as a Warning and nothing else; the golden lines. got_pinned_test.cpp resolves the four relocations, the stub and the
// code words in the real libr15.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/levelname.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_level.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/mapping_perms.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace levelname = nevr_quest::combat::levelname;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using combat_test::Reloc;

// the pinned build's ELF vaddrs (docs/quest/combat/levelname.md)
constexpr std::uintptr_t kFn = 0x1202ecc;
constexpr std::uintptr_t kPlainStub = 0xf133d0;
constexpr std::uintptr_t kSlot[4] = {0x362ef90, 0x3630e58, 0x3631278, 0x36316f8};
constexpr const char* kSlotName[4] = {"cr15_game", "dedicated_server", "lan_server", "net_server"};
constexpr std::uint64_t kGlobalMp = 0x3F9915D3001DC28EULL;
constexpr std::size_t kFlag = 0x7ad8;
constexpr const char* kSymbol = "_ZNK10NRadEngine8NRadGame8CR15Game13LoadLevelNameENS_9CSymbol64E";
constexpr const char* kNoLegacyDir = "/nonexistent-levelname-legacy-dir";  // a directory with no kill file in it

using Fn = std::uint64_t (*)(void*, std::uint64_t);

// ---- the game's side ------------------------------------------------------------------------------------------------------------

// The real function appends "_lowspec" (modelled as a xor), the plain one returns the name.
int g_realCalls = 0, g_plainCalls = 0;
void *g_realGame = nullptr, *g_plainGame = nullptr;
std::uint64_t g_realName = 0, g_plainName = 0;
std::uint64_t FakeReal(void* game, std::uint64_t name) {
  ++g_realCalls;
  g_realGame = game;
  g_realName = name;
  return name ^ 0xa5a5ULL;
}
std::uint64_t FakePlain(void* game, std::uint64_t name) {
  ++g_plainCalls;
  g_plainGame = game;
  g_plainName = name;
  return name;
}
// Four originals that tell the slots apart: each xors a different constant.
template <int I>
std::uint64_t FakeRealOf(void* game, std::uint64_t name) {
  ++g_realCalls;
  g_realGame = game;
  g_realName = name;
  return name ^ (0x100ULL * (I + 1));
}

alignas(16) unsigned char g_game[0x8000];
void SetFlags(std::uint32_t v, std::size_t off = kFlag) {
  std::memset(g_game, 0, sizeof g_game);
  std::memcpy(g_game + off, &v, sizeof v);
}

// ---- the libr15 image -----------------------------------------------------------------------------------------------------------

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

void BuildImage() {
  std::vector<Reloc> relocs;
  for (const std::uintptr_t slot : kSlot) relocs.push_back({kSymbol, slot, false, false, kFn, /*abs64=*/true});
  g_image.Build(relocs);
  for (const std::uintptr_t s : kSlot) {
    g_image.Put<std::uintptr_t>(s, g_image.Base() + kFn);
    g_image.Put<std::uintptr_t>(s - 8, 0x1111);  // neighbours, never touched
    g_image.Put<std::uintptr_t>(s + 8, 0x2222);
  }
}
std::uintptr_t Word(std::size_t i) { return g_image.Get<std::uintptr_t>(kSlot[i]); }

levelname::targets::Sig* const kOriginals[4] = {&FakeRealOf<0>, &FakeRealOf<1>, &FakeRealOf<2>, &FakeRealOf<3>};

// The thunk's entry for slot `i`, as the game calls it.
std::uint64_t CallSlot(std::size_t i, void* game, std::uint64_t name) { return reinterpret_cast<Fn>(Word(i))(game, name); }

void Begin() {
  g_lines.clear();
  levelname::ResetForTest();
  levelname::SetImageLookupForTest(&Lookup);
  nevr_quest::combat::legacy::SetDirectoryForTest(kNoLegacyDir);  // no kill file unless a test puts one in the directory
  g_realCalls = g_plainCalls = 0;
  g_realGame = g_plainGame = nullptr;
  g_realName = g_plainName = 0;
  SetFlags(0x80000000U);
  sentinel::SetLogSink(&Capture);
  BuildImage();
}
void End() {
  levelname::ResetForTest();
  levelname::SetImageLookupForTest(nullptr);
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

// An install, and the fakes standing in for the game's two callees: the originals the manager published (the slot's word, base + the function, is
// zeros in the image) and the plain stub.
nevr_quest::combat::ModuleResult InstallWithFakes(bool fourDistinct = false) {
  const nevr_quest::combat::ModuleResult r = levelname::InstallAt(&Lookup, true);
  if (!r.installed) return r;
  *levelname::targets::Thunk<0>::OriginalOut() = reinterpret_cast<void*>(fourDistinct ? kOriginals[0] : &FakeReal);
  *levelname::targets::Thunk<1>::OriginalOut() = reinterpret_cast<void*>(fourDistinct ? kOriginals[1] : &FakeReal);
  *levelname::targets::Thunk<2>::OriginalOut() = reinterpret_cast<void*>(fourDistinct ? kOriginals[2] : &FakeReal);
  *levelname::targets::Thunk<3>::OriginalOut() = reinterpret_cast<void*>(fourDistinct ? kOriginals[3] : &FakeReal);
  levelname::SetPlainForTest(reinterpret_cast<std::uintptr_t>(&FakePlain));
  return r;
}

// ---- tests ----------------------------------------------------------------------------------------------------------------------

void TheConstantsAreTheStoreLibr15s() {
  namespace t = levelname::targets;
  QCHECK(t::kFnVaddr == kFn && t::kPlainStubVaddr == kPlainStub && t::kGlobalMp == kGlobalMp && t::kFlagOffset == kFlag);
  QCHECK(t::kSlotCount == 4);
  for (std::size_t i = 0; i < 4; ++i) QCHECK(t::kSlots[i] == kSlot[i] && std::strcmp(t::kSlotNames[i], kSlotName[i]) == 0);
  QCHECK(std::strcmp(t::kSymbol, kSymbol) == 0);
  QCHECK(t::kLowspecMask == 0x80000000U);
}

// All four words (and only those) hold their thunk after the install; the callee addresses are base + the vaddrs; the line says so.
void AnInstallPutsAThunkInEachOfTheFourWords() {
  Begin();
  const nevr_quest::combat::ModuleResult r = levelname::InstallAt(&Lookup, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(levelname::Installed());
  QCHECK(Word(0) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<0>::EntryAddress()));
  QCHECK(Word(1) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<1>::EntryAddress()));
  QCHECK(Word(2) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<2>::EntryAddress()));
  QCHECK(Word(3) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<3>::EntryAddress()));
  for (const std::uintptr_t s : kSlot) {
    QCHECK(g_image.Get<std::uintptr_t>(s - 8) == 0x1111 && g_image.Get<std::uintptr_t>(s + 8) == 0x2222);
  }
  const std::uintptr_t real = g_image.Base() + kFn;
  QCHECK(reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<0>::Original()) == real);
  QCHECK(reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<3>::Original()) == real);
  QCHECK(levelname::PlainForTest() == g_image.Base() + kPlainStub);
  const std::vector<Line> install = Lines("combat_levelname_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  QCHECK(Golden(install[0].text) ==
         "\"event\":\"combat_levelname_install\",\"result\":\"installed\",\"reason\":\"ok\",\"key\":\"combat.levelname\",\"slots\":4,"
         "\"failed_slot\":\"none\",\"cr15_game\":\"ok\",\"dedicated_server\":\"ok\",\"lan_server\":\"ok\",\"net_server\":\"ok\","
         "\"rolled_back\":0,\"rollback_failed\":0,\"consequence\":\"global_mp keeps its plain name when the lowspec flag is set: the full-size level "
         "loads, on a map or a lobby of either game\"}");
  End();
}

// The rule, through each of the four words: global_mp with the lowspec bit set is answered by the plain function with the game's arguments;
// anything else is the word's own original with the game's arguments.
void TheRuleRunsThroughEachWord() {
  Begin();
  QCHECK(InstallWithFakes(/*fourDistinct=*/true).installed);
  for (std::size_t i = 0; i < 4; ++i) {
    g_realCalls = g_plainCalls = 0;
    SetFlags(0x80000000U);
    std::uint64_t a = CallSlot(i, g_game, kGlobalMp);  // global_mp + lowspec: the plain name
    QCHECK(a == kGlobalMp && g_plainCalls == 1 && g_realCalls == 0);
    QCHECK(g_plainGame == g_game && g_plainName == kGlobalMp);
    g_realCalls = g_plainCalls = 0;
    const std::uint64_t other = 0x9a3717b576c23967ULL;  // the cut global_mp's own name: any other level
    a = CallSlot(i, g_game, other);
    QCHECK(a == (other ^ (0x100ULL * (i + 1))) && g_realCalls == 1 && g_plainCalls == 0);  // this word's own original answered
    QCHECK(g_realGame == g_game && g_realName == other);
    g_realCalls = g_plainCalls = 0;
    SetFlags(0);  // lowspec clear: the original (it answers plain by itself)
    a = CallSlot(i, g_game, kGlobalMp);
    QCHECK(g_realCalls == 1 && g_plainCalls == 0 && a == (kGlobalMp ^ (0x100ULL * (i + 1))));
  }
  End();
}

// Each part of the rule alone.
void EachPartOfTheRuleAlone() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  g_realCalls = g_plainCalls = 0;
  SetFlags(0x7fffffffU);  // every bit but 31
  CallSlot(0, g_game, kGlobalMp);
  QCHECK(g_realCalls == 1 && g_plainCalls == 0);
  g_realCalls = g_plainCalls = 0;
  SetFlags(0x80000000U, kFlag - 4);  // bit 31 of the neighbour word only
  CallSlot(0, g_game, kGlobalMp);
  SetFlags(0x80000000U, kFlag + 4);
  CallSlot(0, g_game, kGlobalMp);
  QCHECK(g_realCalls == 2 && g_plainCalls == 0);  // the flag is read from game+0x7ad8 and nowhere else
  g_realCalls = g_plainCalls = 0;
  SetFlags(0x80000000U);
  CallSlot(0, g_game, kGlobalMp ^ 1);  // a near miss of the key
  CallSlot(0, g_game, kGlobalMp ^ 0x8000000000000000ULL);
  CallSlot(0, g_game, 0);
  QCHECK(g_realCalls == 3 && g_plainCalls == 0);
  g_realCalls = g_plainCalls = 0;
  SetFlags(0xffffffffU);
  CallSlot(0, g_game, kGlobalMp);
  QCHECK(g_plainCalls == 1 && g_realCalls == 0);
  End();
}

// A handler whose plain function is not published (the install failed after the thunk was armed, or a reset) passes global_mp to the original.
void ANullPlainFunctionPassesTheCallThrough() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  levelname::SetPlainForTest(0);
  g_realCalls = g_plainCalls = 0;
  const std::uint64_t a = CallSlot(2, g_game, kGlobalMp);
  QCHECK(g_realCalls == 1 && g_plainCalls == 0 && a == (kGlobalMp ^ 0xa5a5ULL));
  End();
}

// An unreadable game object never faults: the original answers.
void AnUnreadableGameNeverFaults() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  g_realCalls = g_plainCalls = 0;
  const std::uint64_t a = CallSlot(1, nullptr, kGlobalMp);
  QCHECK(g_realCalls == 1 && g_plainCalls == 0 && a == (kGlobalMp ^ 0xa5a5ULL));
  g_realCalls = g_plainCalls = 0;
  CallSlot(1, reinterpret_cast<void*>(0x7ffffffff000ULL), kGlobalMp);  // an address in no mapping of this process
  QCHECK(g_realCalls == 1 && g_plainCalls == 0);
  End();
}

// An arena match pays nothing, by count (docs/adr/0046; FOLD-MOVE "Prove arena pays nothing, by count"): a level that is not global_mp, from any of the
// four words, makes no read of game memory and no scan of the levels. Arena and combat run on the same APK, so this is what an arena player's level
// loads cost the module: the handler compares the name and calls the original. (global_mp itself, which an arena player loads once after login, reads
// the flags word once: TheCounterAndTheFirstSixteenLines.)
void AnArenaLevelLoadMakesNoReadAndNoScan() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  std::atomic<std::uint64_t> reads{0};
  nevr_quest::combat::SetReadCounterForTest(&reads);
  const std::uint64_t scans0 = nevr_quest::combat::level::ScanCalls();
  for (const std::uint64_t arena : {0x576ed3f8428ebc4bULL, 0x7691d78d414c5701ULL, 0x576ed3f8428ebc48ULL, 0x3f20b99c97955721ULL}) {  // mpl_arena_a, _a_lowspec, _b, mpl_tutorial_a
    for (std::size_t slot = 0; slot < 4; ++slot) QCHECK(CallSlot(slot, g_game, arena) == (arena ^ 0xa5a5ULL));  // the original's answer, untouched
  }
  QCHECK(reads.load() == 0);
  QCHECK(nevr_quest::combat::level::ScanCalls() == scans0);
  CallSlot(0, g_game, kGlobalMp);  // the control: global_mp does read (the flags word, once), so the counter can move
  QCHECK(reads.load() == 1);
  nevr_quest::combat::SetReadCounterForTest(nullptr);
  End();
}

// One Info line per global_mp substitution (the first 16), none for other levels, and the counter keeps counting.
void TheCounterAndTheFirstSixteenLines() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  g_lines.clear();
  for (int i = 0; i < 1000; ++i) CallSlot(static_cast<std::size_t>(i & 3), g_game, 0x1234 + static_cast<std::uint64_t>(i));
  QCHECK(g_lines.empty());  // an ordinary level logged nothing
  QCHECK(levelname::KeptPlainCount() == 0);
  CallSlot(0, g_game, kGlobalMp);
  QCHECK(levelname::KeptPlainCount() == 1);
  const std::vector<Line> first = Lines("combat_levelname_kept");
  QCHECK(first.size() == 1U && first[0].level == sentinel::LogLevel::kInfo);
  QCHECK(Golden(first[0].text) ==
         "\"event\":\"combat_levelname_kept\",\"name\":\"0x3f9915d3001dc28e\",\"n\":1,\"lowspec\":1,\"answer\":\"plain name\"}");
  for (int i = 0; i < 40; ++i) CallSlot(2, g_game, kGlobalMp);
  QCHECK(Lines("combat_levelname_kept").size() == 16U);  // bounded to the first 16
  // the 17th substitution writes one Warning that names the kind and the cap (line_budget.h); the count goes on
  const std::vector<Line> capped = Lines("combat_levelname_log_cap");
  QCHECK(capped.size() == 1U && capped[0].level == sentinel::LogLevel::kWarn);
  QCHECK(capped.size() == 1U && Has(capped[0].text, "\"lines\":\"kept\",\"cap\":16"));
  QCHECK(levelname::KeptPlainCount() == 41);
  SetFlags(0);
  const std::size_t before = g_lines.size();
  CallSlot(0, g_game, kGlobalMp);
  QCHECK(g_lines.size() == before && levelname::KeptPlainCount() == 41);  // lowspec clear is not a substitution
  End();
}

// A word that does not hold the function: the module is refused, nothing stays installed, and the line names the word and the status; once the
// word is right the install can be tried again.
void AWordThatIsNotTheFunctionRefusesAllFourAndPutsBackTheRest() {
  for (std::size_t bad = 0; bad < 4; ++bad) {
    Begin();
    g_image.Put<std::uintptr_t>(kSlot[bad], g_image.Base() + kFn + 4);  // some other function
    const nevr_quest::combat::ModuleResult r = levelname::InstallAt(&Lookup, true);
    QCHECK(!r.installed && std::strcmp(r.status, "refused") == 0 && !levelname::Installed());
    for (std::size_t i = 0; i < 4; ++i) {
      const std::uintptr_t want = i == bad ? g_image.Base() + kFn + 4 : g_image.Base() + kFn;
      QCHECK(Word(i) == want);  // the words before it were put back, the ones after were never touched
    }
    const std::vector<Line> install = Lines("combat_levelname_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
    QCHECK(Has(install[0].text, "\"result\":\"refused\"") && Has(install[0].text, "\"reason\":\"original_mismatch\"") &&
           Has(install[0].text, std::string("\"failed_slot\":\"") + kSlotName[bad] + "\"") && Has(install[0].text, "\"slots\":0"));
    QCHECK(Has(install[0].text, std::string("\"rolled_back\":") + std::to_string(bad) + ",\"rollback_failed\":0"));
    QCHECK(Has(install[0].text, std::string("\"") + kSlotName[bad] + "\":\"original_mismatch\""));
    // a word the module never tried says so
    if (bad < 3) QCHECK(Has(install[0].text, std::string("\"") + kSlotName[3] + "\":\"not_tried\""));
    // handlers are disarmed: a call through a word the manager put back is not ours, and a thunk called by hand passes through
    QCHECK(levelname::PlainForTest() == 0);
    {  // a thunk entry called by hand with a plain function in place: the handler is disarmed, so the original answers
      *levelname::targets::Thunk<0>::OriginalOut() = reinterpret_cast<void*>(&FakeReal);
      levelname::SetPlainForTest(reinterpret_cast<std::uintptr_t>(&FakePlain));
      g_realCalls = g_plainCalls = 0;
      reinterpret_cast<Fn>(levelname::targets::Thunk<0>::EntryAddress())(g_game, kGlobalMp);
      QCHECK(g_realCalls == 1 && g_plainCalls == 0);
      levelname::SetPlainForTest(0);
    }
    if (bad == 3) std::printf("record: %s\n", Golden(install[0].text).c_str());
    // the refused install leaves the retry possible once the word is right
    g_image.Put<std::uintptr_t>(kSlot[bad], g_image.Base() + kFn);
    g_lines.clear();
    const nevr_quest::combat::ModuleResult again = levelname::InstallAt(&Lookup, true);
    QCHECK(again.installed && levelname::Installed());
    End();
  }
}

// A word another writer changed while the module was rolling back cannot be put back by it: the manager leaves that word alone, and the line counts it
// apart from the ones that went back. The observer plays the other writer: it changes slot 0 just before the module's store that would restore it.
std::uintptr_t g_observedSlot = 0, g_observedRestore = 0;
void OtherWriter(void** slot, void* value, sentinel::StorePhase phase) {
  if (phase == sentinel::StorePhase::kBeforeStore && reinterpret_cast<std::uintptr_t>(slot) == g_observedSlot &&
      reinterpret_cast<std::uintptr_t>(value) == g_observedRestore) {
    *slot = reinterpret_cast<void*>(0x4242420);
  }
}
void ARollbackThatCannotPutAWordBackSaysSo() {
  Begin();
  g_image.Put<std::uintptr_t>(kSlot[3], g_image.Base() + kFn + 4);  // the fourth word fails, so words 0 to 2 are rolled back
  g_observedSlot = g_image.Base() + kSlot[0];
  g_observedRestore = g_image.Base() + kFn;
  const sentinel::StoreObserver previous = sentinel::SetStoreObserver(&OtherWriter);
  const nevr_quest::combat::ModuleResult r = levelname::InstallAt(&Lookup, true);
  sentinel::SetStoreObserver(previous);
  QCHECK(!r.installed && !levelname::Installed());
  const std::vector<Line> install = Lines("combat_levelname_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"rolled_back\":2,\"rollback_failed\":1"));
  QCHECK(Word(0) == 0x4242420);  // the other writer's word is left as it is
  QCHECK(Word(1) == g_image.Base() + kFn && Word(2) == g_image.Base() + kFn);
  End();
}

// The other refusals: no libr15, a second install, a different build.
void TheOtherRefusalsSayWhy() {
  Begin();
  nevr_quest::combat::ModuleResult r = levelname::InstallAt(&NoLibr15, true);
  QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
  r = levelname::InstallAt(nullptr, true);
  QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
  const std::vector<Line> refused = Lines("combat_levelname_install");
  QCHECK(refused.size() == 2U && refused[0].level == sentinel::LogLevel::kError && Has(refused[0].text, "\"reason\":\"module_not_loaded\""));
  QCHECK(Word(0) == g_image.Base() + kFn);

  r = levelname::InstallAt(&Lookup, true);
  QCHECK(r.installed);
  g_lines.clear();
  r = levelname::InstallAt(&Lookup, true);  // a second install is refused and writes nothing
  QCHECK(!r.installed && std::strcmp(r.status, "already_ran") == 0);
  QCHECK(Lines("combat_levelname_refused").size() == 1U && Lines("combat_levelname_refused")[0].level == sentinel::LogLevel::kWarn);
  QCHECK(Word(0) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<0>::EntryAddress()));
  QCHECK(Word(3) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<3>::EntryAddress()));
  End();

  Begin();
  g_image.Build({{kSymbol, kSlot[0], false, false, kFn, true},
                 {kSymbol, kSlot[1], false, false, kFn, true},
                 {kSymbol, kSlot[2], false, false, kFn, true},
                 {kSymbol, kSlot[3], false, false, kFn, true}},
                "0000000000000000000000000000000000000001");  // not the patched libr15
  for (const std::uintptr_t s : kSlot) g_image.Put<std::uintptr_t>(s, g_image.Base() + kFn);
  r = levelname::InstallAt(&Lookup, true);
  QCHECK(!r.installed && std::strcmp(r.status, "refused") == 0);
  QCHECK(Has(Lines("combat_levelname_install")[0].text, "\"reason\":\"build_id_mismatch\""));
  QCHECK(Word(0) == g_image.Base() + kFn && Word(3) == g_image.Base() + kFn);
  End();
}

// combat.levelname false: nothing is written, one Info line says why.
void TheSwitchTurnsTheModuleOffAndNothingIsWritten() {
  Begin();
  const nevr_quest::combat::ModuleResult r = levelname::InstallAt(&Lookup, false);
  QCHECK(!r.installed && std::strcmp(r.status, "off") == 0 && !levelname::Installed());
  for (std::size_t i = 0; i < 4; ++i) QCHECK(Word(i) == g_image.Base() + kFn);
  const std::vector<Line> install = Lines("combat_levelname_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  QCHECK(Has(install[0].text, "\"result\":\"off\"") && Has(install[0].text, "\"reason\":\"off_config\"") && Has(install[0].text, "\"key\":\"combat.levelname\""));
  End();
}

// The plugin's kill file `levelname.off` turns nothing off: one found is a Warning that names the key, and the module installs all the same.
void TheLegacyKillFileIsAWarningAndNothingElse() {
  char dir[] = "/tmp/levelname-legacy-XXXXXX";
  const bool made = mkdtemp(dir) != nullptr;
  QCHECK(made);
  if (!made) return;
  Begin();
  nevr_quest::combat::legacy::SetDirectoryForTest(dir);
  nevr_quest::combat::ModuleResult r = levelname::InstallAt(&Lookup, true);
  QCHECK(r.installed && Lines("combat_levelname_legacy_switch").empty());  // no file: nothing said
  levelname::ResetForTest();
  BuildImage();
  const std::string file = std::string(dir) + "/levelname.off";
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  g_lines.clear();
  r = levelname::InstallAt(&Lookup, true);
  QCHECK(r.installed && levelname::Installed());  // the file did not turn it off
  const std::vector<Line> legacy = Lines("combat_levelname_legacy_switch");
  QCHECK(legacy.size() == 1U && legacy[0].level == sentinel::LogLevel::kWarn);
  QCHECK(Has(legacy[0].text, "\"file\":\"levelname.off\"") && Has(legacy[0].text, "\"replaced_by\":\"combat.levelname: false\""));
  // with the key false the file is still told about, and the module stays off
  levelname::ResetForTest();
  BuildImage();
  g_lines.clear();
  r = levelname::InstallAt(&Lookup, false);
  QCHECK(!r.installed && Lines("combat_levelname_legacy_switch").size() == 1U);
  // a directory without the file: nothing is said
  g_lines.clear();
  nevr_quest::combat::legacy::SetDirectoryForTest(kNoLegacyDir);
  r = levelname::InstallAt(&Lookup, false);
  QCHECK(Lines("combat_levelname_legacy_switch").empty());
  End();
  unlink(file.c_str());
  rmdir(dir);
}

// The end-of-process line (#2181): every substitution counted, not only the first sixteen. Written by the table's end step, and only when the module
// is installed.
void TheStatsLineCountsEveryKeptLoadAtTheEnd() {
  Begin();
  levelname::NoteEnd();  // not installed: nothing is written
  QCHECK(Lines("combat_levelname_stats").empty());
  QCHECK(InstallWithFakes().installed);
  g_lines.clear();
  for (int i = 0; i < 20; ++i) CallSlot(0, g_game, kGlobalMp);
  QCHECK(Lines("combat_levelname_stats").empty());  // the line is the end's, not a call's
  levelname::NoteEnd();
  const std::vector<Line> stats = Lines("combat_levelname_stats");
  QCHECK(stats.size() == 1U && stats[0].level == sentinel::LogLevel::kInfo);
  QCHECK(stats.size() == 1U && Has(stats[0].text, "\"kept_plain\":20,\"final\":1,\"consequence\":\""));
  End();
}

// A read-only vtable page (RELRO after relocation): the word is stored and the page is read-only again afterwards.
void AReadOnlyVtablePageIsWrittenAndRestored() {
  Begin();
  const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  for (const std::uintptr_t s : kSlot) mprotect(reinterpret_cast<void*>((g_image.Base() + s) & ~(page - 1)), page, PROT_READ);
  const nevr_quest::combat::ModuleResult r = levelname::InstallAt(&Lookup, true);
  QCHECK(r.installed);
  QCHECK(Word(0) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<0>::EntryAddress()));
  QCHECK(Word(3) == reinterpret_cast<std::uintptr_t>(levelname::targets::Thunk<3>::EntryAddress()));
  for (const std::uintptr_t s : kSlot) QCHECK(!sentinel::MappingsAllow(g_image.Base() + s, sizeof(std::uintptr_t), /*write=*/true));
  for (const std::uintptr_t s : kSlot) mprotect(reinterpret_cast<void*>((g_image.Base() + s) & ~(page - 1)), page, PROT_READ | PROT_WRITE);
  End();
}

}  // namespace

int main() {
  TheConstantsAreTheStoreLibr15s();
  AnInstallPutsAThunkInEachOfTheFourWords();
  TheRuleRunsThroughEachWord();
  EachPartOfTheRuleAlone();
  ANullPlainFunctionPassesTheCallThrough();
  AnUnreadableGameNeverFaults();
  TheCounterAndTheFirstSixteenLines();
  AnArenaLevelLoadMakesNoReadAndNoScan();
  AWordThatIsNotTheFunctionRefusesAllFourAndPutsBackTheRest();
  ARollbackThatCannotPutAWordBackSaysSo();
  TheOtherRefusalsSayWhy();
  TheSwitchTurnsTheModuleOffAndNothingIsWritten();
  TheLegacyKillFileIsAWarningAndNothingElse();
  TheStatsLineCountsEveryKeptLoadAtTheEnd();
  AReadOnlyVtablePageIsWrittenAndRestored();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "levelname_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("levelname_test: ok\n");
  return 0;
}
